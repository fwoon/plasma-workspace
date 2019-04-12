/*
 * Copyright 2019 Kai Uwe Broulik <kde@privat.broulik.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3, or any
 * later version accepted by the membership of KDE e.V. (or its
 * successor approved by the membership of KDE e.V.), which shall
 * act as a proxy defined in Section 6 of version 3 of the license.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "jobsmodel_p.h"

#include "debug.h"

#include "job.h"
#include "job_p.h"

#include "utils_p.h"

#include "kuiserveradaptor.h"
#include "jobviewserveradaptor.h"
#include "jobviewserverv2adaptor.h"

#include <QDBusConnection>
#include <QDBusServiceWatcher>

#include <KJob>
#include <KLocalizedString>
#include <KService>

#include <algorithm>

using namespace NotificationManager;

JobsModelPrivate::JobsModelPrivate(QObject *parent)
    : QObject(parent)
    , m_compressUpdatesTimer(new QTimer(this))
    , m_pendingJobViewsTimer(new QTimer(this))
{
    m_compressUpdatesTimer->setInterval(0);
    m_compressUpdatesTimer->setSingleShot(true);
    connect(m_compressUpdatesTimer, &QTimer::timeout, this, [this] {
        for (auto it = m_pendingDirtyRoles.constBegin(), end = m_pendingDirtyRoles.constEnd(); it != end; ++it) {
            Job *job = it.key();
            const QVector<int> roles = it.value();
            const int row = m_jobViews.indexOf(job);
            if (row == -1) {
                continue;
            }

            emit jobViewChanged(row, job, roles);

            // This is updated here and not the percentageChanged signal so we also get some batching out of it
            if (roles.contains(Notifications::PercentageRole)) {
                updateApplicationPercentage(job->desktopEntry());
            }
        }

        m_pendingDirtyRoles.clear();
    });

    m_pendingJobViewsTimer->setInterval(500);
    m_pendingJobViewsTimer->setSingleShot(true);
    connect(m_pendingJobViewsTimer, &QTimer::timeout, this, [this] {
        const auto pendingJobs = m_pendingJobViews;
        for (Job *job : pendingJobs) {
            if (job->state() == Notifications::JobStateStopped) {
                // Stop finished or canceled in the meantime, remove
                qCDebug(NOTIFICATIONMANAGER) << "By the time we wanted to show JobView" << job->id() << "from" << job->applicationName() << ", it was already stopped";
                remove(job);
                continue;
            }

            const int newRow = m_jobViews.count();
            emit jobViewAboutToBeAdded(newRow, job);
            m_jobViews.append(job);
            emit jobViewAdded(newRow, job);
            updateApplicationPercentage(job->desktopEntry());
        }

        m_pendingJobViews.clear();
    });
}

JobsModelPrivate::~JobsModelPrivate()
{
    QDBusConnection::sessionBus().unregisterService(QStringLiteral("org.kde.JobViewServer"));
    QDBusConnection::sessionBus().unregisterService(QStringLiteral("org.kde.kuiserver"));
    QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/JobViewServer"));

    qDeleteAll(m_jobViews);
    m_jobViews.clear();
    qDeleteAll(m_pendingJobViews);
    m_pendingJobViews.clear();

    m_pendingDirtyRoles.clear();
}

bool JobsModelPrivate::init()
{
    if (m_valid) {
        return true;
    }

    new KuiserverAdaptor(this);
    new JobViewServerAdaptor(this);
    new JobViewServerV2Adaptor(this);

    QDBusConnection sessionBus = QDBusConnection::sessionBus();

    if (!sessionBus.registerObject(QStringLiteral("/JobViewServer"), this)) {
        qCWarning(NOTIFICATIONMANAGER) << "Failed to register JobViewServer DBus object";
        return false;
    }

    if (sessionBus.registerService(QStringLiteral("org.kde.JobViewServer"))) {
        qCDebug(NOTIFICATIONMANAGER) << "Registered JobViewServer service on DBus";
    } else {
        qCWarning(NOTIFICATIONMANAGER) << "Failed to register JobViewServer service on DBus, is kuiserver running?";
        return false;
    }

    if (!sessionBus.registerService(QStringLiteral("org.kde.kuiserver"))) {
        qCWarning(NOTIFICATIONMANAGER) << "Failed to register org.kde.kuiserver service on DBus, is kuiserver running?";
        return false;
    }

    m_serviceWatcher = new QDBusServiceWatcher(this);
    m_serviceWatcher->setConnection(sessionBus);
    m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, &JobsModelPrivate::onServiceUnregistered);

    m_valid = true;
    return true;
}

void JobsModelPrivate::registerService(const QString &service, const QString &objectPath)
{
    qCWarning(NOTIFICATIONMANAGER) << "Request to register JobView service" << service << "on" << objectPath;
    qCWarning(NOTIFICATIONMANAGER) << "org.kde.kuiserver registerService is deprecated and defunct.";
    sendErrorReply(QDBusError::NotSupported, QStringLiteral("kuiserver proxying capabilities are deprecated and defunct."));
}

QStringList JobsModelPrivate::jobUrls() const
{
    QStringList jobUrls;
    for (Job *job : m_jobViews) {
        if (job->state() != Notifications::JobStateStopped && job->destUrl().isValid()) {
            jobUrls.append(job->destUrl().toString());
        }
    }
    for (Job *job : m_pendingJobViews) {
        if (job->state() != Notifications::JobStateStopped && job->destUrl().isValid()) {
            jobUrls.append(job->destUrl().toString());
        }
    }
    return jobUrls;
}

void JobsModelPrivate::emitJobUrlsChanged()
{
    emit jobUrlsChanged(jobUrls());
}

bool JobsModelPrivate::requiresJobTracker() const
{
    return false;
}

QStringList JobsModelPrivate::registeredJobContacts() const
{
    return QStringList();
}

QDBusObjectPath JobsModelPrivate::requestView(const QString &appName, const QString &appIconName, int capabilities)
{
    QString desktopEntry;
    QString applicationName = appName;
    QString applicationIconName = appIconName;

    // JobViewServerV1 only sends application name, try to look it up as a service
    KService::Ptr service = KService::serviceByStorageId(applicationName);
    if (!service) {
        // HACK :)
        service = KService::serviceByStorageId(QStringLiteral("org.kde.") + appName);
    }

    if (service) {
        desktopEntry = service->desktopEntryName();
        applicationName = service->name();
        applicationIconName = service->icon();
    }

    return requestView(desktopEntry, applicationName, applicationIconName, capabilities, QVariantMap() /*hints*/);
}

QDBusObjectPath JobsModelPrivate::requestView(const QString &desktopEntry,
                                                  const QString &appName,
                                                  const QString &appIconName,
                                                  int capabilities,
                                                  const QVariantMap &hints)
{
    Q_UNUSED(hints); // reserved for future extension)

    qCDebug(NOTIFICATIONMANAGER) << "JobView requested by" << desktopEntry << "claiming to be" << appName;

    if (!m_highestJobId) {
        ++m_highestJobId;
    }

    Job *job = new Job(m_highestJobId);
    ++m_highestJobId;

    const QString serviceName = message().service();

    job->setDesktopEntry(desktopEntry);
    job->setApplicationName(appName);
    job->setApplicationIconName(appIconName);

    // No application name? Try to figure out the process name using the sender's PID
    if (job->applicationName().isEmpty()) {
        qCInfo(NOTIFICATIONMANAGER) << "JobView request from" << serviceName << "didn't contain any identification information, this is an application bug!";
        const QString processName = Utils::processNameFromDBusService(connection(), serviceName);
        if (!processName.isEmpty()) {
            qCDebug(NOTIFICATIONMANAGER) << "Resolved JobView request to be from" << processName;
            job->setApplicationName(processName);
        }
    }

    job->setSuspendable(capabilities & KJob::Suspendable);
    job->setKillable(capabilities & KJob::Killable);

    connect(job, &Job::updatedChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::UpdatedRole);
    });
    connect(job, &Job::summaryChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::SummaryRole);
    });
    connect(job, &Job::stateChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::JobStateRole);
        // Timeout and Closable depend on state, signal a change for those, too
        scheduleUpdate(job, Notifications::TimeoutRole);
        scheduleUpdate(job, Notifications::ClosableRole);

        if (job->state() == Notifications::JobStateStopped) {
            updateApplicationPercentage(job->desktopEntry());
            emitJobUrlsChanged();
        }
    });
    connect(job, &Job::percentageChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::PercentageRole);
    });
    connect(job, &Job::errorChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::JobErrorRole);
    });
    connect(job, &Job::expiredChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::ExpiredRole);
    });
    connect(job, &Job::dismissedChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::DismissedRole);
    });

    // The following are used in generating the pretty job text
    connect(job, &Job::processedFilesChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::BodyRole);
    });
    connect(job, &Job::totalFilesChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::BodyRole);
    });
    connect(job, &Job::descriptionValue1Changed, this, [this, job] {
        scheduleUpdate(job, Notifications::BodyRole);
    });
    connect(job, &Job::descriptionValue2Changed, this, [this, job] {
        scheduleUpdate(job, Notifications::BodyRole);
    });
    connect(job, &Job::destUrlChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::BodyRole);
        emitJobUrlsChanged();
    });
    connect(job, &Job::errorTextChanged, this, [this, job] {
        scheduleUpdate(job, Notifications::BodyRole);
    });

    connect(job->d, &JobPrivate::closed, this, [this, job] {
        remove(job);
    });

    // Delay showing a job view by 500ms to avoid showing really short stat jobs and other useless stuff
    if (hints.value(QStringLiteral("immediate")).toBool()) {
        const int newRow = m_jobViews.count();
        emit jobViewAboutToBeAdded(newRow, job);
        m_jobViews.append(job);
        emit jobViewAdded(newRow, job);
        updateApplicationPercentage(job->desktopEntry());
    } else {
        m_pendingJobViews.append(job);
        m_pendingJobViewsTimer->start();
    }

    m_jobServices.insert(job, serviceName);
    m_serviceWatcher->addWatchedService(serviceName);

    return job->d->objectPath();
}

void JobsModelPrivate::remove(Job *job)
{
    const int row = m_jobViews.indexOf(job);
    if (row > -1) {
        removeAt(row);
    }
}

void JobsModelPrivate::removeAt(int row)
{
    Q_ASSERT(row >= 0 && row < m_jobViews.count());

    emit jobViewAboutToBeRemoved(row);//, job);
    Job *job = m_jobViews.takeAt(row);
    m_pendingDirtyRoles.remove(job);
    m_pendingJobViews.removeOne(job);

    const QString serviceName = m_jobServices.take(job);

    // Check if there's any jobs left for this service, otherwise stop watching it
    auto it = std::find_if(m_jobServices.constBegin(), m_jobServices.constEnd(), [&serviceName](const QString &item) {
        return item == serviceName;
    });
    if (it == m_jobServices.constEnd()) {
        m_serviceWatcher->removeWatchedService(serviceName);
    }

    delete job;
    emit jobViewRemoved(row);
}

// This will forward overall application process via Unity API.
// This way users of that like Task Manager and Latte Dock still get basic job information.
void JobsModelPrivate::updateApplicationPercentage(const QString &desktopEntry)
{
    if (desktopEntry.isEmpty()) {
        return;
    }

    int jobsPercentages = 0;
    int jobsCount = 0;

    for (int i = 0; i < m_jobViews.count(); ++i) {
        Job *job = m_jobViews.at(i);
        if (job->state() != Notifications::JobStateStopped) {
            jobsPercentages += job->percentage();
            ++jobsCount;
        }
    }

    int percentage = 0;
    if (jobsCount > 0) {
        percentage = jobsPercentages / jobsCount;
    }

    const QVariantMap properties = {
        {QStringLiteral("count-visible"), jobsCount > 0},
        {QStringLiteral("count"), jobsCount},
        {QStringLiteral("progress-visible"), jobsCount > 0},
        {QStringLiteral("progress"), percentage / 100.0}
    };

    QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/org/kde/notificationmanager/jobs"),
                                                      QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                                      QStringLiteral("Update"));
    message.setArguments({QStringLiteral("application://") + desktopEntry, properties});
    QDBusConnection::sessionBus().send(message);
}

void JobsModelPrivate::onServiceUnregistered(const QString &serviceName)
{
    qCDebug(NOTIFICATIONMANAGER) << "JobView service unregistered" << serviceName;

    m_serviceWatcher->removeWatchedService(serviceName);

    const QList<Job *> jobs = m_jobServices.keys(serviceName);
    for (Job *job : jobs) {
        // Mark all jobs as failed
        job->setError(127); // KIO::ERR_SLAVE_DIED
        job->setErrorText(i18n("Application closed unexpectedly."));
        job->setState(Notifications::JobStateStopped);
    }
}

void JobsModelPrivate::scheduleUpdate(Job *job, Notifications::Roles role)
{
    m_pendingDirtyRoles[job].append(role);
    m_compressUpdatesTimer->start();
}