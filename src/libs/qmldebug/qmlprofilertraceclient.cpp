/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2012 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
**
** GNU Lesser General Public License Usage
**
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this file.
** Please review the following information to ensure the GNU Lesser General
** Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** Other Usage
**
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**************************************************************************/

#include "qmlprofilertraceclient.h"

namespace QmlDebug {

class QmlProfilerTraceClientPrivate {
public:
    QmlProfilerTraceClientPrivate(QmlProfilerTraceClient *_q)
        : q(_q)
        ,  inProgressRanges(0)
        , maximumTime(0)
        , recording(false)
    {
        ::memset(rangeCount, 0, MaximumQmlEventType * sizeof(int));
    }

    void sendRecordingStatus();

    QmlProfilerTraceClient *q;
    qint64 inProgressRanges;
    QStack<qint64> rangeStartTimes[MaximumQmlEventType];
    QStack<QStringList> rangeDatas[MaximumQmlEventType];
    QStack<QmlEventLocation> rangeLocations[MaximumQmlEventType];
    QStack<BindingType> bindingTypes;
    int rangeCount[MaximumQmlEventType];
    qint64 maximumTime;
    bool recording;
};

} // namespace QmlDebug

using namespace QmlDebug;

static const int GAP_TIME = 150;

void QmlProfilerTraceClientPrivate::sendRecordingStatus()
{
    QByteArray ba;
    QDataStream stream(&ba, QIODevice::WriteOnly);
    stream << recording;
    q->sendMessage(ba);
}

QmlProfilerTraceClient::QmlProfilerTraceClient(QmlDebugConnection *client)
    : QmlDebugClient(QLatin1String("CanvasFrameRate"), client)
    , d(new QmlProfilerTraceClientPrivate(this))
{
}

QmlProfilerTraceClient::~QmlProfilerTraceClient()
{
    //Disable profiling if started by client
    //Profiling data will be lost!!
    if (isRecording())
        setRecording(false);
    delete d;
}

void QmlProfilerTraceClient::clearData()
{
    ::memset(d->rangeCount, 0, MaximumQmlEventType * sizeof(int));
    for (int eventType = 0; eventType < MaximumQmlEventType; eventType++) {
        d->rangeDatas[eventType].clear();
        d->rangeLocations[eventType].clear();
        d->rangeStartTimes[eventType].clear();
    }
    d->bindingTypes.clear();
    emit cleared();
}

void QmlProfilerTraceClient::sendRecordingStatus()
{
    d->sendRecordingStatus();
}

bool QmlProfilerTraceClient::isEnabled() const
{
    return status() == Enabled;
}

bool QmlProfilerTraceClient::isRecording() const
{
    return d->recording;
}

void QmlProfilerTraceClient::setRecording(bool v)
{
    if (v == d->recording)
        return;

    d->recording = v;

    if (status() == Enabled) {
        sendRecordingStatus();
    }

    emit recordingChanged(v);
}

void QmlProfilerTraceClient::setRecordingFromServer(bool v)
{
    if (v == d->recording)
        return;
    d->recording = v;
    emit recordingChanged(v);
}

void QmlProfilerTraceClient::statusChanged(Status /*status*/)
{
    emit enabledChanged();
}

void QmlProfilerTraceClient::messageReceived(const QByteArray &data)
{
    QByteArray rwData = data;
    QDataStream stream(&rwData, QIODevice::ReadOnly);

    qint64 time;
    int messageType;

    stream >> time >> messageType;

    if (messageType >= MaximumMessage)
        return;

    if (time > (d->maximumTime + GAP_TIME) && 0 == d->inProgressRanges)
        emit gap(time);

    if (messageType == Event) {
        int event;
        stream >> event;

        // stop with the first data
        if (d->recording && event != StartTrace)
            setRecordingFromServer(false);
        else if ((!d->recording) && event == StartTrace)
            setRecordingFromServer(true);

        if (event == EndTrace) {
            emit this->traceFinished(time);
            d->maximumTime = time;
            d->maximumTime = qMax(time, d->maximumTime);
        } else if (event == AnimationFrame) {
            int frameRate, animationCount;
            stream >> frameRate >> animationCount;
            emit this->frame(time, frameRate, animationCount);
            d->maximumTime = qMax(time, d->maximumTime);
        } else if (event == StartTrace) {
            emit this->traceStarted(time);
            d->maximumTime = time;
        } else if (event < MaximumEventType) {
            emit this->event((EventType)event, time);
            d->maximumTime = qMax(time, d->maximumTime);
        }
    } else if (messageType == Complete) {
        emit complete();
    } else {
        int range;
        stream >> range;

        if (range >= MaximumQmlEventType)
            return;

        if (messageType == RangeStart) {
            d->rangeStartTimes[range].push(time);
            d->inProgressRanges |= (static_cast<qint64>(1) << range);
            ++d->rangeCount[range];

            // read binding type
            if ((QmlEventType)range == Binding) {
                int bindingType = (int)QmlBinding;
                if (!stream.atEnd())
                    stream >> bindingType;
                d->bindingTypes.push((BindingType)bindingType);
            }

            // stop with the first data
            if (d->recording)
                setRecordingFromServer(false);
        } else if (messageType == RangeData) {
            QString data;
            stream >> data;

            int count = d->rangeCount[range];
            if (count > 0) {
                while (d->rangeDatas[range].count() < count)
                    d->rangeDatas[range].push(QStringList());
                d->rangeDatas[range][count-1] << data;
            }

        } else if (messageType == RangeLocation) {
            QString fileName;
            int line;
            int column = -1;
            stream >> fileName >> line;

            if (!stream.atEnd())
                stream >> column;

            if (d->rangeCount[range] > 0) {
                d->rangeLocations[range].push(QmlEventLocation(fileName, line, column));
            }
        } else {
            if (d->rangeCount[range] > 0) {
                --d->rangeCount[range];
                if (d->inProgressRanges & (static_cast<qint64>(1) << range))
                    d->inProgressRanges &= ~(static_cast<qint64>(1) << range);

                d->maximumTime = qMax(time, d->maximumTime);
                QStringList data = d->rangeDatas[range].count() ? d->rangeDatas[range].pop() : QStringList();
                QmlEventLocation location = d->rangeLocations[range].count() ? d->rangeLocations[range].pop() : QmlEventLocation();

                qint64 startTime = d->rangeStartTimes[range].pop();
                BindingType bindingType = QmlBinding;
                if ((QmlEventType)range == Binding)
                    bindingType = d->bindingTypes.pop();
                emit this->range((QmlEventType)range, bindingType, startTime, time - startTime, data, location);
                if (d->rangeCount[range] == 0) {
                    int count = d->rangeDatas[range].count() +
                                d->rangeStartTimes[range].count() +
                                d->rangeLocations[range].count();
                    if (count != 0)
                        qWarning() << "incorrectly nested data";
                }
            }
        }
    }
}
