/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <math.h>
#include <sys/types.h>

#include <cutils/properties.h>

#include <utils/SortedVector.h>
#include <utils/KeyedVector.h>
#include <utils/threads.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Singleton.h>
#include <utils/String16.h>

#include <binder/BinderService.h>
#include <binder/IServiceManager.h>
#include <binder/PermissionCache.h>

#include <gui/ISensorServer.h>
#include <gui/ISensorEventConnection.h>
#include <gui/SensorEventQueue.h>

#include <hardware/sensors.h>
#include <hardware_legacy/power.h>

#include "BatteryService.h"
#include "CorrectedGyroSensor.h"
#include "GravitySensor.h"
#include "LinearAccelerationSensor.h"
#include "OrientationSensor.h"
#include "RotationVectorSensor.h"
#include "SensorFusion.h"
#include "SensorService.h"

namespace android {
// ---------------------------------------------------------------------------

/*
 * Notes:
 *
 * - what about a gyro-corrected magnetic-field sensor?
 * - run mag sensor from time to time to force calibration
 * - gravity sensor length is wrong (=> drift in linear-acc sensor)
 *
 */

const char* SensorService::WAKE_LOCK_NAME = "SensorService";

SensorService::SensorService()
    : mInitCheck(NO_INIT)
{
}

void SensorService::onFirstRef()
{
    ALOGD("nuSensorService starting...");

    SensorDevice& dev(SensorDevice::getInstance());

    if (dev.initCheck() == NO_ERROR) {
        sensor_t const* list;
        ssize_t count = dev.getSensorList(&list);
        if (count > 0) {
            ssize_t orientationIndex = -1;
            bool hasGyro = false;
            uint32_t virtualSensorsNeeds =
                    (1<<SENSOR_TYPE_GRAVITY) |
                    (1<<SENSOR_TYPE_LINEAR_ACCELERATION) |
                    (1<<SENSOR_TYPE_ROTATION_VECTOR);

            mLastEventSeen.setCapacity(count);
            for (ssize_t i=0 ; i<count ; i++) {
                registerSensor( new HardwareSensor(list[i]) );
                switch (list[i].type) {
                    case SENSOR_TYPE_ORIENTATION:
                        orientationIndex = i;
                        break;
                    case SENSOR_TYPE_GYROSCOPE:
                    case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
                        hasGyro = true;
                        break;
                    case SENSOR_TYPE_GRAVITY:
                    case SENSOR_TYPE_LINEAR_ACCELERATION:
                    case SENSOR_TYPE_ROTATION_VECTOR:
                        virtualSensorsNeeds &= ~(1<<list[i].type);
                        break;
                }
            }

            // it's safe to instantiate the SensorFusion object here
            // (it wants to be instantiated after h/w sensors have been
            // registered)
            const SensorFusion& fusion(SensorFusion::getInstance());

            // build the sensor list returned to users
            mUserSensorList = mSensorList;

            if (hasGyro) {
                Sensor aSensor;

                // Add Android virtual sensors if they're not already
                // available in the HAL

                aSensor = registerVirtualSensor( new RotationVectorSensor() );
                if (virtualSensorsNeeds & (1<<SENSOR_TYPE_ROTATION_VECTOR)) {
                    mUserSensorList.add(aSensor);
                }

                aSensor = registerVirtualSensor( new GravitySensor(list, count) );
                if (virtualSensorsNeeds & (1<<SENSOR_TYPE_GRAVITY)) {
                    mUserSensorList.add(aSensor);
                }

                aSensor = registerVirtualSensor( new LinearAccelerationSensor(list, count) );
                if (virtualSensorsNeeds & (1<<SENSOR_TYPE_LINEAR_ACCELERATION)) {
                    mUserSensorList.add(aSensor);
                }

                aSensor = registerVirtualSensor( new OrientationSensor() );
                if (virtualSensorsNeeds & (1<<SENSOR_TYPE_ROTATION_VECTOR)) {
                    // if we are doing our own rotation-vector, also add
                    // the orientation sensor and remove the HAL provided one.
                    mUserSensorList.replaceAt(aSensor, orientationIndex);
                }

                // virtual debugging sensors are not added to mUserSensorList
                registerVirtualSensor( new CorrectedGyroSensor(list, count) );
                registerVirtualSensor( new GyroDriftSensor() );
            }

            // debugging sensor list
            mUserSensorListDebug = mSensorList;

            run("SensorService", PRIORITY_URGENT_DISPLAY);
            mInitCheck = NO_ERROR;
        }
    }
}

Sensor SensorService::registerSensor(SensorInterface* s)
{
    sensors_event_t event;
    memset(&event, 0, sizeof(event));

    const Sensor sensor(s->getSensor());
    // add to the sensor list (returned to clients)
    mSensorList.add(sensor);
    // add to our handle->SensorInterface mapping
    mSensorMap.add(sensor.getHandle(), s);
    // create an entry in the mLastEventSeen array
    mLastEventSeen.add(sensor.getHandle(), event);

    return sensor;
}

Sensor SensorService::registerVirtualSensor(SensorInterface* s)
{
    Sensor sensor = registerSensor(s);
    mVirtualSensorList.add( s );
    return sensor;
}

SensorService::~SensorService()
{
    for (size_t i=0 ; i<mSensorMap.size() ; i++)
        delete mSensorMap.valueAt(i);
}

static const String16 sDump("android.permission.DUMP");

status_t SensorService::dump(int fd, const Vector<String16>& args)
{
    String8 result;
    if (!PermissionCache::checkCallingPermission(sDump)) {
        result.appendFormat("Permission Denial: "
                "can't dump SurfaceFlinger from pid=%d, uid=%d\n",
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid());
    } else {
        Mutex::Autolock _l(mLock);
        result.append("Sensor List:\n");
        for (size_t i=0 ; i<mSensorList.size() ; i++) {
            const Sensor& s(mSensorList[i]);
            const sensors_event_t& e(mLastEventSeen.valueFor(s.getHandle()));
            result.appendFormat(
                    "%-48s| %-32s | 0x%08x | ",
                    s.getName().string(),
                    s.getVendor().string(),
                    s.getHandle());

            if (s.getMinDelay() > 0) {
                result.appendFormat(
                    "maxRate=%7.2fHz | ", 1e6f / s.getMinDelay());
            } else {
                result.append(s.getMinDelay() == 0
                        ? "on-demand         | "
                        : "one-shot          | ");
            }

            switch (s.getType()) {
                case SENSOR_TYPE_ROTATION_VECTOR:
                case SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR:
                    result.appendFormat(
                            "last=<%5.1f,%5.1f,%5.1f,%5.1f,%5.1f>\n",
                            e.data[0], e.data[1], e.data[2], e.data[3], e.data[4]);
                    break;
                case SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED:
                case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
                    result.appendFormat(
                            "last=<%5.1f,%5.1f,%5.1f,%5.1f,%5.1f,%5.1f>\n",
                            e.data[0], e.data[1], e.data[2], e.data[3], e.data[4], e.data[5]);
                    break;
                case SENSOR_TYPE_GAME_ROTATION_VECTOR:
                    result.appendFormat(
                            "last=<%5.1f,%5.1f,%5.1f,%5.1f>\n",
                            e.data[0], e.data[1], e.data[2], e.data[3]);
                    break;
                case SENSOR_TYPE_SIGNIFICANT_MOTION:
                case SENSOR_TYPE_STEP_DETECTOR:
                    result.appendFormat( "last=<%f>\n", e.data[0]);
                    break;
                case SENSOR_TYPE_STEP_COUNTER:
                    result.appendFormat( "last=<%llu>\n", e.u64.step_counter);
                    break;
                default:
                    // default to 3 values
                    result.appendFormat(
                            "last=<%5.1f,%5.1f,%5.1f>\n",
                            e.data[0], e.data[1], e.data[2]);
                    break;
            }
        }
        SensorFusion::getInstance().dump(result);
        SensorDevice::getInstance().dump(result);

        result.appendFormat("%d active connections\n", mActiveConnections.size());
        result.append("Active sensors:\n");
        for (size_t i=0 ; i<mActiveSensors.size() ; i++) {
            int handle = mActiveSensors.keyAt(i);
            result.appendFormat("%s (handle=0x%08x, connections=%d)\n",
                    getSensorName(handle).string(),
                    handle,
                    mActiveSensors.valueAt(i)->getNumConnections());
        }
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

void SensorService::cleanupAutoDisabledSensor(const sp<SensorEventConnection>& connection,
        sensors_event_t const* buffer, const int count) {
    SensorInterface* sensor;
    status_t err = NO_ERROR;
    for (int i=0 ; i<count ; i++) {
        int handle = buffer[i].sensor;
        int type = buffer[i].type;
        if (type == SENSOR_TYPE_SIGNIFICANT_MOTION) {
            if (connection->hasSensor(handle)) {
                sensor = mSensorMap.valueFor(handle);
                if (sensor != NULL) {
                    sensor->autoDisable(connection.get(), handle);
                }
                cleanupWithoutDisable(connection, handle);
            }
        }
    }
}

bool SensorService::threadLoop()
{
    ALOGD("nuSensorService thread starting...");

    const size_t numEventMax = 16;
    const size_t minBufferSize = numEventMax + numEventMax * mVirtualSensorList.size();
    sensors_event_t buffer[minBufferSize];
    sensors_event_t scratch[minBufferSize];
    SensorDevice& device(SensorDevice::getInstance());
    const size_t vcount = mVirtualSensorList.size();

    ssize_t count;
    bool wakeLockAcquired = false;
    const int halVersion = device.getHalDeviceVersion();
    do {
        count = device.poll(buffer, numEventMax);
        if (count<0) {
            ALOGE("sensor poll failed (%s)", strerror(-count));
            break;
        }

        // Poll has returned. Hold a wakelock.
        // Todo(): add a flag to the sensors definitions to indicate
        // the sensors which can wake up the AP
        for (int i = 0; i < count; i++) {
            if (buffer[i].type == SENSOR_TYPE_SIGNIFICANT_MOTION) {
                 acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_NAME);
                 wakeLockAcquired = true;
                 break;
            }
        }

        recordLastValue(buffer, count);

        // handle virtual sensors
        if (count && vcount) {
            sensors_event_t const * const event = buffer;
            const DefaultKeyedVector<int, SensorInterface*> virtualSensors(
                    getActiveVirtualSensors());
            const size_t activeVirtualSensorCount = virtualSensors.size();
            if (activeVirtualSensorCount) {
                size_t k = 0;
                SensorFusion& fusion(SensorFusion::getInstance());
                if (fusion.isEnabled()) {
                    for (size_t i=0 ; i<size_t(count) ; i++) {
                        fusion.process(event[i]);
                    }
                }
                for (size_t i=0 ; i<size_t(count) && k<minBufferSize ; i++) {
                    for (size_t j=0 ; j<activeVirtualSensorCount ; j++) {
                        if (count + k >= minBufferSize) {
                            ALOGE("buffer too small to hold all events: "
                                    "count=%u, k=%u, size=%u",
                                    count, k, minBufferSize);
                            break;
                        }
                        sensors_event_t out;
                        SensorInterface* si = virtualSensors.valueAt(j);
                        if (si->process(&out, event[i])) {
                            buffer[count + k] = out;
                            k++;
                        }
                    }
                }
                if (k) {
                    // record the last synthesized values
                    recordLastValue(&buffer[count], k);
                    count += k;
                    // sort the buffer by time-stamps
                    sortEventBuffer(buffer, count);
                }
            }
        }

        // handle backward compatibility for RotationVector sensor
        if (halVersion < SENSORS_DEVICE_API_VERSION_1_0) {
            for (int i = 0; i < count; i++) {
                if (buffer[i].type == SENSOR_TYPE_ROTATION_VECTOR) {
                    // All the 4 components of the quaternion should be available
                    // No heading accuracy. Set it to -1
                    buffer[i].data[4] = -1;
                }
            }
        }

        // send our events to clients...
        const SortedVector< wp<SensorEventConnection> > activeConnections(
                getActiveConnections());
        size_t numConnections = activeConnections.size();
        for (size_t i=0 ; i<numConnections ; i++) {
            sp<SensorEventConnection> connection(
                    activeConnections[i].promote());
            if (connection != 0) {
                connection->sendEvents(buffer, count, scratch);
                // Some sensors need to be auto disabled after the trigger
                cleanupAutoDisabledSensor(connection, buffer, count);
            }
        }

        // We have read the data, upper layers should hold the wakelock.
        if (wakeLockAcquired) release_wake_lock(WAKE_LOCK_NAME);

    } while (count >= 0 || Thread::exitPending());

    ALOGW("Exiting SensorService::threadLoop => aborting...");
    abort();
    return false;
}

void SensorService::recordLastValue(
        sensors_event_t const * buffer, size_t count)
{
    Mutex::Autolock _l(mLock);

    // record the last event for each sensor
    int32_t prev = buffer[0].sensor;
    for (size_t i=1 ; i<count ; i++) {
        // record the last event of each sensor type in this buffer
        int32_t curr = buffer[i].sensor;
        if (curr != prev) {
            mLastEventSeen.editValueFor(prev) = buffer[i-1];
            prev = curr;
        }
    }
    mLastEventSeen.editValueFor(prev) = buffer[count-1];
}

void SensorService::sortEventBuffer(sensors_event_t* buffer, size_t count)
{
    struct compar {
        static int cmp(void const* lhs, void const* rhs) {
            sensors_event_t const* l = static_cast<sensors_event_t const*>(lhs);
            sensors_event_t const* r = static_cast<sensors_event_t const*>(rhs);
            return l->timestamp - r->timestamp;
        }
    };
    qsort(buffer, count, sizeof(sensors_event_t), compar::cmp);
}

SortedVector< wp<SensorService::SensorEventConnection> >
SensorService::getActiveConnections() const
{
    Mutex::Autolock _l(mLock);
    return mActiveConnections;
}

DefaultKeyedVector<int, SensorInterface*>
SensorService::getActiveVirtualSensors() const
{
    Mutex::Autolock _l(mLock);
    return mActiveVirtualSensors;
}

String8 SensorService::getSensorName(int handle) const {
    size_t count = mUserSensorList.size();
    for (size_t i=0 ; i<count ; i++) {
        const Sensor& sensor(mUserSensorList[i]);
        if (sensor.getHandle() == handle) {
            return sensor.getName();
        }
    }
    String8 result("unknown");
    return result;
}

Vector<Sensor> SensorService::getSensorList()
{
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.sensors", value, "0");
    if (atoi(value)) {
        return mUserSensorListDebug;
    }
    return mUserSensorList;
}

sp<ISensorEventConnection> SensorService::createSensorEventConnection()
{
    uid_t uid = IPCThreadState::self()->getCallingUid();
    sp<SensorEventConnection> result(new SensorEventConnection(this, uid));
    return result;
}

void SensorService::cleanupConnection(SensorEventConnection* c)
{
    Mutex::Autolock _l(mLock);
    const wp<SensorEventConnection> connection(c);
    size_t size = mActiveSensors.size();
    ALOGD_IF(DEBUG_CONNECTIONS, "%d active sensors", size);
    for (size_t i=0 ; i<size ; ) {
        int handle = mActiveSensors.keyAt(i);
        if (c->hasSensor(handle)) {
            ALOGD_IF(DEBUG_CONNECTIONS, "%i: disabling handle=0x%08x", i, handle);
            SensorInterface* sensor = mSensorMap.valueFor( handle );
            ALOGE_IF(!sensor, "mSensorMap[handle=0x%08x] is null!", handle);
            if (sensor) {
                sensor->activate(c, false);
            }
        }
        SensorRecord* rec = mActiveSensors.valueAt(i);
        ALOGE_IF(!rec, "mActiveSensors[%d] is null (handle=0x%08x)!", i, handle);
        ALOGD_IF(DEBUG_CONNECTIONS,
                "removing connection %p for sensor[%d].handle=0x%08x",
                c, i, handle);

        if (rec && rec->removeConnection(connection)) {
            ALOGD_IF(DEBUG_CONNECTIONS, "... and it was the last connection");
            mActiveSensors.removeItemsAt(i, 1);
            mActiveVirtualSensors.removeItem(handle);
            delete rec;
            size--;
        } else {
            i++;
        }
    }
    mActiveConnections.remove(connection);
    BatteryService::cleanup(c->getUid());
}

status_t SensorService::enable(const sp<SensorEventConnection>& connection,
        int handle)
{
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    SensorInterface* sensor = mSensorMap.valueFor(handle);
    if (sensor == NULL) {
        return BAD_VALUE;
    }

    Mutex::Autolock _l(mLock);
    SensorRecord* rec = mActiveSensors.valueFor(handle);
    if (rec == 0) {
        rec = new SensorRecord(connection);
        mActiveSensors.add(handle, rec);
        if (sensor->isVirtual()) {
            mActiveVirtualSensors.add(handle, sensor);
        }
    } else {
        if (rec->addConnection(connection)) {
            // this sensor is already activated, but we are adding a
            // connection that uses it. Immediately send down the last
            // known value of the requested sensor if it's not a
            // "continuous" sensor.
            if (sensor->getSensor().getMinDelay() == 0) {
                sensors_event_t scratch;
                sensors_event_t& event(mLastEventSeen.editValueFor(handle));
                if (event.version == sizeof(sensors_event_t)) {
                    connection->sendEvents(&event, 1);
                }
            }
        }
    }

    if (connection->addSensor(handle)) {
        BatteryService::enableSensor(connection->getUid(), handle);
        // the sensor was added (which means it wasn't already there)
        // so, see if this connection becomes active
        if (mActiveConnections.indexOf(connection) < 0) {
            mActiveConnections.add(connection);
        }
    } else {
        ALOGW("sensor %08x already enabled in connection %p (ignoring)",
            handle, connection.get());
    }

    // we are setup, now enable the sensor.
    status_t err = sensor->activate(connection.get(), true);
    if (err != NO_ERROR) {
        // enable has failed, reset our state.
        cleanupWithoutDisableLocked(connection, handle);
    }
    return err;
}

status_t SensorService::disable(const sp<SensorEventConnection>& connection,
        int handle)
{
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    Mutex::Autolock _l(mLock);
    status_t err = cleanupWithoutDisableLocked(connection, handle);
    if (err == NO_ERROR) {
        SensorInterface* sensor = mSensorMap.valueFor(handle);
        err = sensor ? sensor->activate(connection.get(), false) : status_t(BAD_VALUE);
    }
    return err;
}

status_t SensorService::cleanupWithoutDisable(
        const sp<SensorEventConnection>& connection, int handle) {
    Mutex::Autolock _l(mLock);
    return cleanupWithoutDisableLocked(connection, handle);
}

status_t SensorService::cleanupWithoutDisableLocked(
        const sp<SensorEventConnection>& connection, int handle) {
    SensorRecord* rec = mActiveSensors.valueFor(handle);
    if (rec) {
        // see if this connection becomes inactive
        if (connection->removeSensor(handle)) {
            BatteryService::disableSensor(connection->getUid(), handle);
        }
        if (connection->hasAnySensor() == false) {
            mActiveConnections.remove(connection);
        }
        // see if this sensor becomes inactive
        if (rec->removeConnection(connection)) {
            mActiveSensors.removeItem(handle);
            mActiveVirtualSensors.removeItem(handle);
            delete rec;
        }
        return NO_ERROR;
    }
    return BAD_VALUE;
}

status_t SensorService::setEventRate(const sp<SensorEventConnection>& connection,
        int handle, nsecs_t ns)
{
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    SensorInterface* sensor = mSensorMap.valueFor(handle);
    if (!sensor)
        return BAD_VALUE;

    if (ns < 0)
        return BAD_VALUE;

    nsecs_t minDelayNs = sensor->getSensor().getMinDelayNs();
    if (ns < minDelayNs) {
        ns = minDelayNs;
    }

    if (ns < MINIMUM_EVENTS_PERIOD)
        ns = MINIMUM_EVENTS_PERIOD;

    return sensor->setDelay(connection.get(), handle, ns);
}

// ---------------------------------------------------------------------------

SensorService::SensorRecord::SensorRecord(
        const sp<SensorEventConnection>& connection)
{
    mConnections.add(connection);
}

bool SensorService::SensorRecord::addConnection(
        const sp<SensorEventConnection>& connection)
{
    if (mConnections.indexOf(connection) < 0) {
        mConnections.add(connection);
        return true;
    }
    return false;
}

bool SensorService::SensorRecord::removeConnection(
        const wp<SensorEventConnection>& connection)
{
    ssize_t index = mConnections.indexOf(connection);
    if (index >= 0) {
        mConnections.removeItemsAt(index, 1);
    }
    return mConnections.size() ? false : true;
}

// ---------------------------------------------------------------------------

SensorService::SensorEventConnection::SensorEventConnection(
        const sp<SensorService>& service, uid_t uid)
    : mService(service), mChannel(new BitTube()), mUid(uid)
{
}

SensorService::SensorEventConnection::~SensorEventConnection()
{
    ALOGD_IF(DEBUG_CONNECTIONS, "~SensorEventConnection(%p)", this);
    mService->cleanupConnection(this);
}

void SensorService::SensorEventConnection::onFirstRef()
{
}

bool SensorService::SensorEventConnection::addSensor(int32_t handle) {
    Mutex::Autolock _l(mConnectionLock);
    if (mSensorInfo.indexOf(handle) < 0) {
        mSensorInfo.add(handle);
        return true;
    }
    return false;
}

bool SensorService::SensorEventConnection::removeSensor(int32_t handle) {
    Mutex::Autolock _l(mConnectionLock);
    if (mSensorInfo.remove(handle) >= 0) {
        return true;
    }
    return false;
}

bool SensorService::SensorEventConnection::hasSensor(int32_t handle) const {
    Mutex::Autolock _l(mConnectionLock);
    return mSensorInfo.indexOf(handle) >= 0;
}

bool SensorService::SensorEventConnection::hasAnySensor() const {
    Mutex::Autolock _l(mConnectionLock);
    return mSensorInfo.size() ? true : false;
}

status_t SensorService::SensorEventConnection::sendEvents(
        sensors_event_t const* buffer, size_t numEvents,
        sensors_event_t* scratch)
{
    // filter out events not for this connection
    size_t count = 0;
    if (scratch) {
        Mutex::Autolock _l(mConnectionLock);
        size_t i=0;
        while (i<numEvents) {
            const int32_t curr = buffer[i].sensor;
            if (mSensorInfo.indexOf(curr) >= 0) {
                do {
                    scratch[count++] = buffer[i++];
                } while ((i<numEvents) && (buffer[i].sensor == curr));
            } else {
                i++;
            }
        }
    } else {
        scratch = const_cast<sensors_event_t *>(buffer);
        count = numEvents;
    }

    // NOTE: ASensorEvent and sensors_event_t are the same type
    ssize_t size = SensorEventQueue::write(mChannel,
            reinterpret_cast<ASensorEvent const*>(scratch), count);
    if (size == -EAGAIN) {
        // the destination doesn't accept events anymore, it's probably
        // full. For now, we just drop the events on the floor.
        //ALOGW("dropping %d events on the floor", count);
        return size;
    }

    return size < 0 ? status_t(size) : status_t(NO_ERROR);
}

sp<BitTube> SensorService::SensorEventConnection::getSensorChannel() const
{
    return mChannel;
}

status_t SensorService::SensorEventConnection::enableDisable(
        int handle, bool enabled)
{
    status_t err;
    if (enabled) {
        err = mService->enable(this, handle);
    } else {
        err = mService->disable(this, handle);
    }
    return err;
}

status_t SensorService::SensorEventConnection::setEventRate(
        int handle, nsecs_t ns)
{
    return mService->setEventRate(this, handle, ns);
}

// ---------------------------------------------------------------------------
}; // namespace android

