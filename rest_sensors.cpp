/*
 * Copyright (c) 2013-2017 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include <QString>
#include <QTextCodec>
#include <QTcpSocket>
#include <QVariantMap>
#include <QtCore/qmath.h>
#include "de_web_plugin.h"
#include "de_web_plugin_private.h"
#include "json.h"

// duration after which the state.presence is turned to 'false'
// if the sensor doesn't trigger
const int MaxOnTimeWithoutPresence = 60 * 6;

/*! Sensors REST API broker.
    \param req - request data
    \param rsp - response data
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::handleSensorsApi(ApiRequest &req, ApiResponse &rsp)
{
    if (req.path[2] != QLatin1String("sensors"))
    {
        return REQ_NOT_HANDLED;
    }

    if(!checkApikeyAuthentification(req, rsp))
    {
        return REQ_READY_SEND;
    }

    // GET /api/<apikey>/sensors
    if ((req.path.size() == 3) && (req.hdr.method() == "GET"))
    {
        return getAllSensors(req, rsp);
    }
    // GET /api/<apikey>/sensors/new
    else if ((req.path.size() == 4) && (req.hdr.method() == "GET") && (req.path[3] == "new"))
    {
        return getNewSensors(req, rsp);
    }
    // GET /api/<apikey>/sensors/<id>
    else if ((req.path.size() == 4) && (req.hdr.method() == "GET"))
    {
        return getSensor(req, rsp);
    }
    // POST /api/<apikey>/sensors
    else if ((req.path.size() == 3) && (req.hdr.method() == "POST"))
    {
        bool ok;
        QVariant var = Json::parse(req.content, ok);
        QVariantMap map = var.toMap();

        if (map.isEmpty())
        {
            return findNewSensors(req, rsp);
        }
        else
        {
            return createSensor(req, rsp);
        }
    }
    // PUT, PATCH /api/<apikey>/sensors/<id>
    else if ((req.path.size() == 4) && (req.hdr.method() == "PUT" || req.hdr.method() == "PATCH"))
    {
        return updateSensor(req, rsp);
    }
    // DELETE /api/<apikey>/sensors/<id>
    else if ((req.path.size() == 4) && (req.hdr.method() == "DELETE"))
    {
        return deleteSensor(req, rsp);
    }
    // PUT, PATCH /api/<apikey>/sensors/<id>/config
    else if ((req.path.size() == 5) && (req.hdr.method() == "PUT" || req.hdr.method() == "PATCH") && (req.path[4] == "config"))
    {
        return changeSensorConfig(req, rsp);
    }
    // PUT, PATCH /api/<apikey>/sensors/<id>/state
    else if ((req.path.size() == 5) && (req.hdr.method() == "PUT" || req.hdr.method() == "PATCH") && (req.path[4] == "state"))
    {
        return changeSensorState(req, rsp);
    }

    return REQ_NOT_HANDLED;
}

/*! GET /api/<apikey>/sensors
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::getAllSensors(const ApiRequest &req, ApiResponse &rsp)
{
    Q_UNUSED(req);
    rsp.httpStatus = HttpStatusOk;

    // handle ETag
    if (req.hdr.hasKey("If-None-Match"))
    {
        QString etag = req.hdr.value("If-None-Match");

        if (gwSensorsEtag == etag)
        {
            rsp.httpStatus = HttpStatusNotModified;
            rsp.etag = etag;
            return REQ_READY_SEND;
        }
    }

    std::vector<Sensor>::iterator i = sensors.begin();
    std::vector<Sensor>::iterator end = sensors.end();

    for (; i != end; ++i)
    {
        // ignore deleted sensors
        if (i->deletedState() == Sensor::StateDeleted)
        {
            continue;
        }

        // ignore sensors without attached node
        if (i->modelId().startsWith("FLS-NB") && !i->node())
        {
            continue;
        }

        if (i->modelId().isEmpty())
        {
            continue;
        }

        QVariantMap map;
        sensorToMap(&*i, map, req.strict);
        rsp.map[i->id()] = map;
    }

    if (rsp.map.isEmpty())
    {
        rsp.str = "{}"; // return empty object
    }

    rsp.etag = gwSensorsEtag;

    return REQ_READY_SEND;
}

/*! GET /api/<apikey>/sensors/<id>
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::getSensor(const ApiRequest &req, ApiResponse &rsp)
{
    DBG_Assert(req.path.size() == 4);

    if (req.path.size() != 4)
    {
        return REQ_NOT_HANDLED;
    }


    const QString &id = req.path[3];

    Sensor *sensor = getSensorNodeForId(id);

    if (!sensor || (sensor->deletedState() == Sensor::StateDeleted))
    {
        rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString("/sensors/%1").arg(id), QString("resource, /sensors/%1, not available").arg(id)));
        rsp.httpStatus = HttpStatusNotFound;
        return REQ_READY_SEND;
    }

    // handle ETag
    if (req.hdr.hasKey("If-None-Match"))
    {
        QString etag = req.hdr.value("If-None-Match");

        if (sensor->etag == etag)
        {
            rsp.httpStatus = HttpStatusNotModified;
            rsp.etag = etag;
            return REQ_READY_SEND;
        }
    }

    sensorToMap(sensor, rsp.map, req.strict);
    rsp.httpStatus = HttpStatusOk;
    rsp.etag = sensor->etag;

    return REQ_READY_SEND;
}

/*! POST /api/<apikey>/sensors
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::createSensor(const ApiRequest &req, ApiResponse &rsp)
{
    rsp.httpStatus = HttpStatusOk;

    bool ok;
    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();
    QString type = map["type"].toString();
    Sensor sensor;

    if (!ok)
    {
        rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/sensors"), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    userActivity();

    if (sensors.size() >= MAX_SENSORS)
    {
        rsp.list.append(errorToMap(ERR_SENSOR_LIST_FULL , QString("/sensors/"), QString("The Sensor List has reached its maximum capacity of %1 sensors").arg(MAX_SENSORS)));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    //check required parameter
    if ((!(map.contains("name")) || !(map.contains("modelid")) || !(map.contains("swversion")) || !(map.contains("type")) || !(map.contains("uniqueid")) || !(map.contains("manufacturername"))))
    {
        rsp.list.append(errorToMap(ERR_MISSING_PARAMETER, QString("/sensors"), QString("invalid/missing parameters in body")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    //check invalid parameter
    QVariantMap::const_iterator pi = map.begin();
    QVariantMap::const_iterator pend = map.end();

    for (; pi != pend; ++pi)
    {
        if(!((pi.key() == "name") || (pi.key() == "modelid") || (pi.key() == "swversion") || (pi.key() == "type")  || (pi.key() == "uniqueid")  || (pi.key() == "manufacturername")  || (pi.key() == "state")  || (pi.key() == "config")))
        {
            rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/%2").arg(pi.key()), QString("parameter, %1, not available").arg(pi.key())));
            rsp.httpStatus = HttpStatusBadRequest;
            return REQ_READY_SEND;
        }
    }

    if (!type.startsWith(QLatin1String("CLIP")))
    {
        rsp.list.append(errorToMap(ERR_NOT_ALLOWED_SENSOR_TYPE, QString("/sensors"), QString("Not allowed to create sensor type")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

        ResourceItem *item;
        QVariantMap rspItem;
        QVariantMap rspItemState;

        // create a new sensor id
        openDb();
        sensor.setId(QString::number(getFreeSensorId()));
        closeDb();

        sensor.setName(map["name"].toString());
        sensor.setManufacturer(map["manufacturername"].toString());
        sensor.setModelId(map["modelid"].toString());
        sensor.setUniqueId(map["uniqueid"].toString());
        sensor.setSwVersion(map["swversion"].toString());
        sensor.setType(type);

        if      (type == QLatin1String("CLIPAlarm")) { item = sensor.addItem(DataTypeBool, RStateAlarm); item->setValue(false); }
        else if (type == QLatin1String("CLIPCarbonMonoxide")) { item = sensor.addItem(DataTypeBool, RStateCarbonMonoxide); item->setValue(false); }
        else if (type == QLatin1String("CLIPConsumption")) { item = sensor.addItem(DataTypeUInt64, RStateConsumption); item->setValue(0); }
        else if (type == QLatin1String("CLIPFire")) { item = sensor.addItem(DataTypeBool, RStateFire); item->setValue(false); }
        else if (type == QLatin1String("CLIPGenericFlag")) { item = sensor.addItem(DataTypeBool, RStateFlag); item->setValue(false); }
        else if (type == QLatin1String("CLIPGenericStatus")) { item = sensor.addItem(DataTypeInt32, RStateStatus); item->setValue(0); }
        else if (type == QLatin1String("CLIPHumidity")) { item = sensor.addItem(DataTypeUInt16, RStateHumidity); item->setValue(0); }
        else if (type == QLatin1String("CLIPLightLevel")) { item = sensor.addItem(DataTypeUInt16, RStateLightLevel); item->setValue(0);
                                                            item = sensor.addItem(DataTypeUInt32, RStateLux); item->setValue(0);
                                                            item = sensor.addItem(DataTypeBool, RStateDark); item->setValue(true);
                                                            item = sensor.addItem(DataTypeBool, RStateDaylight); item->setValue(false);
                                                            item = sensor.addItem(DataTypeUInt16, RConfigTholdDark); item->setValue(R_THOLDDARK_DEFAULT);
                                                            item = sensor.addItem(DataTypeUInt16, RConfigTholdOffset); item->setValue(R_THOLDOFFSET_DEFAULT); }
        else if (type == QLatin1String("CLIPOpenClose")) { item = sensor.addItem(DataTypeBool, RStateOpen); item->setValue(false); }
        else if (type == QLatin1String("CLIPPower")) { item = sensor.addItem(DataTypeInt16, RStatePower); item->setValue(0);
                                                       item = sensor.addItem(DataTypeUInt16, RStateVoltage); item->setValue(0);
                                                       item = sensor.addItem(DataTypeUInt16, RStateCurrent); item->setValue(0); }
        else if (type == QLatin1String("CLIPPresence")) { item = sensor.addItem(DataTypeBool, RStatePresence); item->setValue(false);
                                                          item = sensor.addItem(DataTypeUInt16, RConfigDuration); item->setValue(60); }
        else if (type == QLatin1String("CLIPPressure")) { item = sensor.addItem(DataTypeInt16, RStatePressure); item->setValue(0); }
        else if (type == QLatin1String("CLIPSwitch")) { item = sensor.addItem(DataTypeInt32, RStateButtonEvent); item->setValue(0); }
        else if (type == QLatin1String("CLIPTemperature")) { item = sensor.addItem(DataTypeInt16, RStateTemperature); item->setValue(0);
                                                             item = sensor.addItem(DataTypeInt16, RConfigOffset); item->setValue(0); }
        else if (type == QLatin1String("CLIPWater")) { item = sensor.addItem(DataTypeBool, RStateWater); item->setValue(false); }
        else
        {
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("invalid value, %1, for parameter, type").arg(type)));
            rsp.httpStatus = HttpStatusBadRequest;
            return REQ_READY_SEND;
        }

        //setState optional
        if (map.contains("state"))
        {
            QVariantMap state = map["state"].toMap();

            //check invalid parameter
            QVariantMap::const_iterator pi = state.begin();
            QVariantMap::const_iterator pend = state.end();

            for (; pi != pend; ++pi)
            {
                if(!((pi.key() == "buttonevent") || (pi.key() == "flag") || (pi.key() == "status") || (pi.key() == "presence")  || (pi.key() == "open")  || (pi.key() == "lightlevel") || (pi.key() == "temperature")  || (pi.key() == "humidity")))
                {
                    rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/%2").arg(pi.key()), QString("parameter, %1, not available").arg(pi.key())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }

            if (state.contains("buttonevent"))
            {
                item = sensor.item(RStateButtonEvent);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, buttonevent, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["buttonevent"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter buttonevent").arg(state["buttonevent"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("flag"))
            {
                item = sensor.item(RStateFlag);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, flag, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["flag"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter flag").arg(state["flag"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("status"))
            {
                item = sensor.item(RStateStatus);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, status, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["status"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter status").arg(state["status"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("presence"))
            {
                item = sensor.item(RStatePresence);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, presence, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["presence"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter presence").arg(state["presence"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("open"))
            {
                item = sensor.item(RStateOpen);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, open, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["open"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter open").arg(state["open"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("lightlevel"))
            {
                item = sensor.item(RStateLightLevel);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, lightlevel, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["lightlevel"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter lightlevel").arg(state["lightlevel"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("temperature"))
            {
                item = sensor.item(RStateTemperature);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, temperature, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["temperature"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter temperature").arg(state["temperature"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("humidity"))
            {
                item = sensor.item(RStateHumidity);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, humidity, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["humidity"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter humidity").arg(state["humidity"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("pressure"))
            {
                item = sensor.item(RStatePressure);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, pressure, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["pressure"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter pressure").arg(state["pressure"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("alarm"))
            {
                item = sensor.item(RStateAlarm);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, alarm, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["alarm"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter alarm").arg(state["alarm"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("carbonmonoxide"))
            {
                item = sensor.item(RStateCarbonMonoxide);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, carbonmonoxide, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["carbonmonoxide"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter carbonmonoxide").arg(state["carbonmonoxide"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("fire"))
            {
                item = sensor.item(RStateFire);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, fire, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["fire"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter fire").arg(state["fire"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("water"))
            {
                item = sensor.item(RStateWater);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, water, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["water"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter water").arg(state["water"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("lowbattery"))
            {
                item = sensor.addItem(DataTypeBool, RStateLowBattery);
                if (!item->setValue(state["lowbattery"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter lowbattery").arg(state["lowbattery"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("tampered"))
            {
                item = sensor.addItem(DataTypeBool, RStateTampered);
                if (!item->setValue(state["tampered"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter tampered").arg(state["tampered"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("consumption"))
            {
                item = sensor.item(RStateConsumption);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, consumption, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["consumption"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter consumption").arg(state["consumption"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("power"))
            {
                item = sensor.item(RStatePower);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, power, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["power"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter power").arg(state["power"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("voltage"))
            {
                item = sensor.item(RStateVoltage);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, voltage, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["voltage"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter voltage").arg(state["voltage"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (state.contains("current"))
            {
                item = sensor.item(RStateCurrent);
                if (!item)
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors"), QString("parameter, current, not available")));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }

                if (!item->setValue(state["current"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/state"), QString("invalid value, %1, for parameter current").arg(state["current"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }

        }

        item = sensor.item(RConfigOn);
        item->setValue(true); // default

        item = sensor.item(RConfigReachable);
        item->setValue(true); //default

        //setConfig optional
        if (map.contains("config"))
        {
            QVariantMap config = map["config"].toMap();

            //check invalid parameter
            QVariantMap::const_iterator pi = config.begin();
            QVariantMap::const_iterator pend = config.end();

            for (; pi != pend; ++pi)
            {
                if(!((pi.key() == "offset") || (pi.key() == "on") || (pi.key() == "reachable") || (pi.key() == "url") || (pi.key() == "battery") || (pi.key() == "duration") || (pi.key() == "delay")))
                {
                    rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/%2").arg(pi.key()), QString("parameter, %1, not available").arg(pi.key())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }

            if (config.contains("offset"))
            {
                item = sensor.addItem(DataTypeInt16, RConfigOffset);

                if (!item || !item->setValue(config["offset"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/config"), QString("invalid value, %1, for parameter offset").arg(config["offset"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (config.contains("on"))
            {
                item = sensor.item(RConfigOn);
                item->setValue(config["on"]);
            }
            if (config.contains("reachable"))
            {
                item = sensor.addItem(DataTypeBool, RConfigReachable);
                item->setValue(config["reachable"]);
            }
            if (config.contains("url"))
            {
                item = sensor.addItem(DataTypeString, RConfigUrl);
                item->setValue(config["url"]);
            }
            if (config.contains("battery"))
            {
                item = sensor.addItem(DataTypeUInt8, RConfigBattery);

                if (!item || !item->setValue(config["battery"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/config"), QString("invalid value, %1, for parameter battery").arg(config["battery"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (config.contains("duration"))
            {
                item = sensor.item(RConfigDuration);

                if (!item || !item->setValue(config["duration"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/config"), QString("invalid value, %1, for parameter duration").arg(config["duration"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
            if (config.contains("delay"))
            {
                item = sensor.item(RConfigDelay);

                if (!item || !item->setValue(config["delay"]))
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/config"), QString("invalid value, %1, for parameter delay").arg(config["delay"].toString())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
        }
        updateSensorEtag(&sensor);
        sensor.setNeedSaveDatabase(true);
        sensors.push_back(sensor);
        queSaveDb(DB_SENSORS, DB_SHORT_SAVE_DELAY);

        rspItemState["id"] = sensor.id();
        rspItem["success"] = rspItemState;
        rsp.list.append(rspItem);
        rsp.httpStatus = HttpStatusOk;
        return REQ_READY_SEND;
}

/*! PUT, PATCH /api/<apikey>/sensors/<id>
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::updateSensor(const ApiRequest &req, ApiResponse &rsp)
{
    QString id = req.path[3];
    Sensor *sensor = getSensorNodeForId(id);
    QString name;
    bool ok;
    bool error = false;
    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();
    QVariantMap rspItem;
    QVariantMap rspItemState;

    rsp.httpStatus = HttpStatusOk;

    if (!ok)
    {
        rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/sensors"), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    if (!sensor || (sensor->deletedState() == Sensor::StateDeleted))
    {
        rsp.httpStatus = HttpStatusNotFound;
        rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString("/sensors/%1").arg(id), QString("resource, /sensors/%1, not available").arg(id)));
        return REQ_READY_SEND;
    }

    userActivity();

    //check invalid parameter
    QVariantMap::const_iterator pi = map.begin();
    QVariantMap::const_iterator pend = map.end();

    for (; pi != pend; ++pi)
    {
        if(!((pi.key() == "name") || (pi.key() == "modelid") || (pi.key() == "swversion")
             || (pi.key() == "type")  || (pi.key() == "uniqueid")  || (pi.key() == "manufacturername")
             || (pi.key() == "state")  || (pi.key() == "config") || (pi.key() == "mode" && sensor->modelId() == "Lighting Switch")))
        {
            rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/%2").arg(pi.key()), QString("parameter, %1, not available").arg(pi.key())));
            rsp.httpStatus = HttpStatusBadRequest;
            return REQ_READY_SEND;
        }
    }

    if (map.contains("modelid"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/modelid"), QString("parameter, modelid, not modifiable")));
    }
    if (map.contains("swversion"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/swversion"), QString("parameter, swversion, not modifiable")));
    }
    if (map.contains("type"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/type"), QString("parameter, type, not modifiable")));
    }
    if (map.contains("uniqueid"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/uniqueid"), QString("parameter, uniqueid, not modifiable")));
    }
    if (map.contains("manufacturername"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/manufacturername"), QString("parameter, manufacturername, not modifiable")));
    }
    if (map.contains("state"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/state"), QString("parameter, state, not modifiable")));
    }
    if (map.contains("config"))
    {
        error = true;
        rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/config"), QString("parameter, config, not modifiable")));
    }

    if (error)
    {
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    if (map.contains("name")) // optional
    {
        name = map["name"].toString();

        if ((map["name"].type() == QVariant::String) && !(name.isEmpty()) && (name.size() <= MAX_SENSOR_NAME_LENGTH))
        {
            if (sensor->name() != name)
            {
                sensor->setName(name);
                sensor->setNeedSaveDatabase(true);
                queSaveDb(DB_SENSORS, DB_SHORT_SAVE_DELAY);
                updateSensorEtag(sensor);

                Event e(RSensors, RAttrName, sensor->id(), sensor->item(RAttrName));
                enqueueEvent(e);
            }
            if (!sensor->type().startsWith(QLatin1String("CLIP")))
            {
                pushSensorInfoToCore(sensor);
            }
            rspItemState[QString("/sensors/%1/name:").arg(id)] = name;
            rspItem["success"] = rspItemState;
            rsp.list.append(rspItem);
        }
        else
        {
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/%1/name").arg(id), QString("invalid value, %1, for parameter, /sensors/%2/name").arg(name).arg(id)));
            rsp.httpStatus = HttpStatusBadRequest;
        }
    }

    if (map.contains("mode")) // optional
    {
        Sensor::SensorMode mode = (Sensor::SensorMode)map["mode"].toUInt(&ok);

        if (ok && (map["mode"].type() == QVariant::Double) && (mode == Sensor::ModeScenes || mode == Sensor::ModeTwoGroups || mode == Sensor::ModeColorTemperature))
        {
            if (sensor->mode() != mode)
            {
                sensor->setNeedSaveDatabase(true);
                sensor->setMode(mode);
                queSaveDb(DB_SENSORS, DB_SHORT_SAVE_DELAY);
                updateSensorEtag(sensor);
            }

            rspItemState[QString("/sensors/%1/mode:").arg(id)] = (double)mode;
            rspItem["success"] = rspItemState;
            rsp.list.append(rspItem);
            updateEtag(sensor->etag);
            updateEtag(gwConfigEtag);
            queSaveDb(DB_SENSORS | DB_GROUPS, DB_SHORT_SAVE_DELAY);
        }
        else
        {
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/%1/mode").arg(id), QString("invalid value, %1, for parameter, /sensors/%2/mode").arg((int)mode).arg(id)));
            rsp.httpStatus = HttpStatusBadRequest;
        }
    }

    return REQ_READY_SEND;
}

/*! PUT, PATCH /api/<apikey>/sensors/<id>/config
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::changeSensorConfig(const ApiRequest &req, ApiResponse &rsp)
{
    TaskItem task;
    QString id = req.path[3];
    Sensor *sensor = getSensorNodeForId(id);
    bool ok;
    bool updated = false;
    bool offsetUpdated = false;
    qint16 offset = 0;
    bool tholdUpdated = false;
    uint8_t pendingMask = 0;
    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();
    QVariantMap rspItem;
    QVariantMap rspItemState;

//    QRegExp latitude("^\\d{3,3}\\.\\d{4,4}(W|E)$");
//    QRegExp longitude("^\\d{3,3}\\.\\d{4,4}(N|S)$");

    rsp.httpStatus = HttpStatusOk;

    if (!ok)
    {
        rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/sensors/config"), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    if (!sensor || (sensor->deletedState() == Sensor::StateDeleted))
    {
        rsp.httpStatus = HttpStatusNotFound;
        rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString("/sensors/%1").arg(id), QString("resource, /sensors/%1, not available").arg(id)));
        return REQ_READY_SEND;
    }

    bool isClip = sensor->type().startsWith(QLatin1String("CLIP"));

    userActivity();

    // set destination parameters
    task.req.dstAddress() = sensor->address();
    task.req.setTxOptions(deCONZ::ApsTxAcknowledgedTransmission);
    task.req.setDstEndpoint(sensor->fingerPrint().endpoint);
    task.req.setSrcEndpoint(getSrcEndpoint(sensor, task.req));
    task.req.setDstAddressMode(deCONZ::ApsExtAddress);

    //check invalid parameter
    QVariantMap::const_iterator pi = map.begin();
    QVariantMap::const_iterator pend = map.end();

    for (; pi != pend; ++pi)
    {
        ResourceItemDescriptor rid;
        ResourceItem *item = 0;
        if (getResourceItemDescriptor(QString("config/%1").arg(pi.key()), rid))
        {
            if (!isClip && (rid.suffix == RConfigBattery || rid.suffix == RConfigReachable))
            {
                // changing battery or reachable of zigbee sensors is not allowed, trigger error
            }
            else if (rid.suffix == RConfigPending || rid.suffix == RConfigSensitivityMax)
            {
                // pending and sensitivitymax are read-only
            }
            //else if (rid.suffix == RConfigDuration && sensor->modelId() == QLatin1String("TRADFRI motion sensor"))
            //{
                // duration can be written for ikea motion sensor
                // values 0, 60 — 600 will be replaced by hardware settings TODO error message
            //}
            else
            {
                item = sensor->item(rid.suffix);
            }

            if (item)
            {
                QVariant val = map[pi.key()];

                if (rid.suffix == RConfigOffset)
                {
                    offset -= item->toNumber();
                }
                if (rid.suffix == RConfigAlert)
                {
                    if (val == "none")
                    {
                        task.identifyTime = 0;
                    }
                    else if (val == "select")
                    {
                        task.identifyTime = 2;    // Hue lights don't react to 1.
                    }
                    else if (val == "lselect")
                    {
                        task.identifyTime = 15;   // Default for Philips Hue bridge
                    }
                    else
                    {
                        rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/%1/config/alert").arg(id), QString("invalid value, %1, for parameter, alert").arg(val.toString())));
                        rsp.httpStatus = HttpStatusBadRequest;
                        return REQ_READY_SEND;
                    }

                    task.taskType = TaskIdentify;
                    taskToLocalData(task);

                    if (addTaskIdentify(task, task.identifyTime))
                    {
                        if (item->setValue(val))
                        {
                            rspItemState[QString("/sensors/%1/config/alert").arg(id)] = map["alert"];
                            rspItem["success"] = rspItemState;
                            if (item->lastChanged() == item->lastSet())
                            {
                                updated = true;
                            }
                        }
                    }
                    else
                    {
                        rsp.list.append(errorToMap(ERR_INTERNAL_ERROR, QString("/sensors/%1").arg(id), QString("Internal error, %1").arg(ERR_BRIDGE_BUSY)));
                    }
                }
                else if (item->setValue(val))
                {
                    rspItemState[QString("/sensors/%1/config/%2").arg(id).arg(pi.key())] = val;
                    rspItem["success"] = rspItemState;
                    Event e(RSensors, rid.suffix, id, item);
                    enqueueEvent(e);

                    if (item->lastChanged() == item->lastSet())
                    {
                        updated = true;

                        if (rid.suffix == RConfigTholdDark || rid.suffix == RConfigTholdOffset)
                        {
                            tholdUpdated = true;
                        }
                        else if (rid.suffix == RConfigOffset)
                        {
                            offsetUpdated = true;
                            offset += item->toNumber();
                        }
                        else if (rid.suffix == RConfigDelay && sensor->modelId() == QLatin1String("SML001")) // Hue motion sensor
                        {
                            pendingMask |= R_PENDING_DELAY;
                            sensor->enableRead(WRITE_DELAY);
                            sensor->setNextReadTime(WRITE_DELAY, QTime::currentTime());
                        }
                        else if (rid.suffix == RConfigDuration && sensor->modelId().startsWith(QLatin1String("FLS-NB")))
                        {
                            DBG_Printf(DBG_INFO, "Force read of occupaction delay for sensor %s\n", qPrintable(sensor->address().toStringExt()));
                            sensor->enableRead(READ_OCCUPANCY_CONFIG);
                            sensor->setNextReadTime(READ_OCCUPANCY_CONFIG, queryTime.addSecs(1));
                            queryTime = queryTime.addSecs(1);
                            Q_Q(DeRestPlugin);
                            q->startZclAttributeTimer(0);
                        }
                        else if (rid.suffix == RConfigLedIndication)
                        {
                            pendingMask |= R_PENDING_LEDINDICATION;
                            sensor->enableRead(WRITE_LEDINDICATION);
                            sensor->setNextReadTime(WRITE_LEDINDICATION, QTime::currentTime());
                        }
                        else if (rid.suffix == RConfigSensitivity)
                        {
                            pendingMask |= R_PENDING_SENSITIVITY;
                            sensor->enableRead(WRITE_SENSITIVITY);
                            sensor->setNextReadTime(WRITE_SENSITIVITY, QTime::currentTime());
                        }
                        else if (rid.suffix == RConfigUsertest)
                        {
                            pendingMask |= R_PENDING_USERTEST;
                            sensor->enableRead(WRITE_USERTEST);
                            sensor->setNextReadTime(WRITE_USERTEST, QTime::currentTime());
                        }
                    }
                }
                else // invalid
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/%1/config/%2").arg(id).arg(pi.key()),
                                               QString("invalid value, %1, for parameter %2").arg(val.toString()).arg(pi.key())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
        }

        if (!item)
        {
            // not found
            rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/%1/config/%2").arg(id).arg(pi.key()), QString("parameter, %1, not available").arg(pi.key())));
            rsp.httpStatus = HttpStatusBadRequest;
            return REQ_READY_SEND;
        }
    }

    if (tholdUpdated)
    {
        ResourceItem *item = sensor->item(RStateLightLevel);
        if (item)
        {
            quint16 lightlevel = item->toNumber();

            item = sensor->item(RConfigTholdDark);
            if (item)
            {
                quint16 tholddark = item->toNumber();

                item = sensor->item(RConfigTholdOffset);
                if (item)
                {
                    quint16 tholdoffset = item->toNumber();

                    bool dark = lightlevel <= tholddark;
                    bool daylight = lightlevel >= tholddark + tholdoffset;

                    item = sensor->item(RStateDark);
                    if (!item)
                    {
                        item = sensor->addItem(DataTypeBool, RStateDark);
                    }
                    if (item->setValue(dark))
                    {
                        if (item->lastChanged() == item->lastSet())
                        {
                            Event e(RSensors, RStateDark, sensor->id(), item);
                            enqueueEvent(e);
                        }
                    }

                    item = sensor->item(RStateDaylight);
                    if (!item)
                    {
                        item = sensor->addItem(DataTypeBool, RStateDaylight);
                    }
                    if (item->setValue(daylight))
                    {
                        if (item->lastChanged() == item->lastSet())
                        {
                            Event e(RSensors, RStateDaylight, sensor->id(), item);
                            enqueueEvent(e);
                        }
                    }
                }
            }
        }
    }

    if (offsetUpdated)
    {
        ResourceItem *item = sensor->item(RStateTemperature);
        if (item)
        {
            qint16 temp = item->toNumber();
            temp += offset;
            if (item->setValue(temp)) {
                Event e(RSensors, RStateTemperature, sensor->id(), item);
                enqueueEvent(e);
            }
        }
    }

    if (pendingMask)
    {
      ResourceItem *item = sensor->item(RConfigPending);
      if (item)
      {
          uint8_t mask = item->toNumber();
          mask |= pendingMask;
          item->setValue(mask);
      }
    }

    rsp.list.append(rspItem);
    updateSensorEtag(sensor);

    if (updated)
    {
        sensor->setNeedSaveDatabase(true);
        queSaveDb(DB_SENSORS, DB_SHORT_SAVE_DELAY);
    }

    processTasks();

    return REQ_READY_SEND;
}

/*! PUT, PATCH /api/<apikey>/sensors/<id>/state
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::changeSensorState(const ApiRequest &req, ApiResponse &rsp)
{
    QString id = req.path[3];
    Sensor *sensor = getSensorNodeForId(id);
    bool ok;
    bool updated = false;
    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();
    QVariantMap rspItem;
    QVariantMap rspItemState;

    rsp.httpStatus = HttpStatusOk;

    if (!ok)
    {
        rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/sensors/%1/state").arg(id), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    if (!sensor || (sensor->deletedState() == Sensor::StateDeleted))
    {
        rsp.httpStatus = HttpStatusNotFound;
        rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString("/sensors/%1").arg(id), QString("resource, /sensors/%1, not available").arg(id)));
        return REQ_READY_SEND;
    }

    bool isClip = sensor->type().startsWith(QLatin1String("CLIP"));

    userActivity();

    //check invalid parameter
    QVariantMap::const_iterator pi = map.begin();
    QVariantMap::const_iterator pend = map.end();

    for (; pi != pend; ++pi)
    {
        ResourceItem *item = 0;
        ResourceItemDescriptor rid;
        if (isClip && getResourceItemDescriptor(QString("state/%1").arg(pi.key()), rid))
        {
            if (rid.suffix != RStateLux && rid.suffix != RStateDark && rid.suffix != RStateDaylight)
            {
                item = sensor->item(rid.suffix);
            }
            if (item)
            {
                QVariant val = map[pi.key()];
                if (item->setValue(val))
                {
                    rspItemState[QString("/sensors/%1/state/%2").arg(id).arg(pi.key())] = val;
                    rspItem["success"] = rspItemState;

                    if (rid.suffix == RStateButtonEvent ||  // always fire events for buttons
                        item->lastChanged() == item->lastSet())
                    {
                        updated = true;
                        Event e(RSensors, rid.suffix, id, item);
                        enqueueEvent(e);
                    }
                    sensor->updateStateTimestamp();
                    enqueueEvent(Event(RSensors, RStateLastUpdated, id));

                    if (rid.suffix == RStateLightLevel)
                    {
                        ResourceItem *item2 = 0;
                        quint16 measuredValue = val.toUInt();

                        quint16 tholddark = R_THOLDDARK_DEFAULT;
                        quint16 tholdoffset = R_THOLDOFFSET_DEFAULT;
                        item2 = sensor->item(RConfigTholdDark);
                        if (item2)
                        {
                            tholddark = item2->toNumber();
                        }
                        item2 = sensor->item(RConfigTholdOffset);
                        if (item2)
                        {
                            tholdoffset = item2->toNumber();
                        }
                        bool dark = measuredValue <= tholddark;
                        bool daylight = measuredValue >= tholddark + tholdoffset;

                        item2 = sensor->item(RStateDark);
                        if (!item2)
                        {
                            item2 = sensor->addItem(DataTypeBool, RStateDark);
                        }
                        if (item2->setValue(dark))
                        {
                            if (item2->lastChanged() == item2->lastSet())
                            {
                                Event e(RSensors, RStateDark, id, item2);
                                enqueueEvent(e);
                            }
                        }

                        item2 = sensor->item(RStateDaylight);
                        if (!item2)
                        {
                            item2 = sensor->addItem(DataTypeBool, RStateDaylight);
                        }
                        if (item2->setValue(daylight))
                        {
                            if (item2->lastChanged() == item2->lastSet())
                            {
                                Event e(RSensors, RStateDaylight, id, item2);
                                enqueueEvent(e);
                            }
                        }

                        item2 = sensor->item(RStateLux);
                        if (!item2)
                        {
                            item2 = sensor->addItem(DataTypeUInt32, RStateLux);
                        }
                        quint32 lux = 0;
                        if (measuredValue > 0 && measuredValue < 0xffff)
                        {
                            // valid values are 1 - 0xfffe
                            // 0, too low to measure
                            // 0xffff invalid value

                            // ZCL Attribute = 10.000 * log10(Illuminance (lx)) + 1
                            // lux = 10^((ZCL Attribute - 1)/10.000)
                            qreal exp = measuredValue - 1;
                            qreal l = qPow(10, exp / 10000.0f);
                            l += 0.5;   // round value
                            lux = static_cast<quint32>(l);
                        }
                        item2->setValue(lux);
                        if (item2->lastChanged() == item2->lastSet())
                        {
                            Event e(RSensors, RStateLux, id, item2);
                            enqueueEvent(e);
                        }
                    }
                    else if (rid.suffix == RStatePresence)
                    {
                        ResourceItem *item2 = sensor->item(RConfigDuration);
                        if (item2 && item2->toNumber() > 0)
                        {
                            sensor->durationDue = QDateTime::currentDateTime().addSecs(item2->toNumber()).addMSecs(-500);
                        }
                    }
                }
                else // invalid
                {
                    rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/%1/state/%2").arg(id).arg(pi.key()),
                                               QString("invalid value, %1, for parameter %2").arg(val.toString()).arg(pi.key())));
                    rsp.httpStatus = HttpStatusBadRequest;
                    return REQ_READY_SEND;
                }
            }
        }

        if (!item)
        {
            // not found
            rsp.list.append(errorToMap(ERR_PARAMETER_NOT_AVAILABLE, QString("/sensors/%1/state/%2").arg(id).arg(pi.key()), QString("parameter, %1, not available").arg(pi.key())));
            rsp.httpStatus = HttpStatusBadRequest;
            return REQ_READY_SEND;
        }
    }

    rsp.list.append(rspItem);
    updateSensorEtag(sensor);
    if (updated)
    {
        sensor->setNeedSaveDatabase(true);
        queSaveDb(DB_SENSORS, DB_HUGE_SAVE_DELAY);
    }

    return REQ_READY_SEND;
}

/*! DELETE /api/<apikey>/sensors/<id>
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::deleteSensor(const ApiRequest &req, ApiResponse &rsp)
{
    QString id = req.path[3];
    Sensor *sensor = getSensorNodeForId(id);

    userActivity();

    if (!sensor || (sensor->deletedState() == Sensor::StateDeleted))
    {
        rsp.httpStatus = HttpStatusNotFound;
        rsp.list.append(errorToMap(ERR_RESOURCE_NOT_AVAILABLE, QString("/sensors/%1").arg(id), QString("resource, /sensors/%1, not available").arg(id)));
        return REQ_READY_SEND;
    }

    bool ok;
    QVariant var = Json::parse(req.content, ok);
    QVariantMap map = var.toMap();

    if (!ok)
    {
        rsp.list.append(errorToMap(ERR_INVALID_JSON, QString("/sensors/%1").arg(id), QString("body contains invalid JSON")));
        rsp.httpStatus = HttpStatusBadRequest;
        return REQ_READY_SEND;
    }

    sensor->setDeletedState(Sensor::StateDeleted);
    sensor->setNeedSaveDatabase(true);

    Event e(RSensors, REventDeleted, sensor->id());
    enqueueEvent(e);

    bool hasReset = map.contains("reset");

    if (hasReset)
    {
        if (map["reset"].type() == QVariant::Bool)
        {
            bool reset = map["reset"].toBool();

            QVariantMap rspItem;
            QVariantMap rspItemState;
            rspItemState[QString("/sensors/%1/reset").arg(id)] = reset;
            rspItem["success"] = rspItemState;
            rsp.list.append(rspItem);

            if (reset)
            {
                sensor->setResetRetryCount(10);
            }
        }
        else
        {
            rsp.list.append(errorToMap(ERR_INVALID_VALUE, QString("/sensors/%1/reset").arg(id), QString("invalid value, %1, for parameter, reset").arg(map["reset"].toString())));
            rsp.httpStatus = HttpStatusBadRequest;
            return REQ_READY_SEND;
        }
    }
    else
    {
        QVariantMap rspItem;
        QVariantMap rspItemState;
        rspItemState["id"] = id;
        rspItem["success"] = rspItemState;
        rsp.list.append(rspItem);
        rsp.httpStatus = HttpStatusOk;
    }

    queSaveDb(DB_SENSORS, DB_SHORT_SAVE_DELAY);

    updateSensorEtag(sensor);
    rsp.httpStatus = HttpStatusOk;

    return REQ_READY_SEND;
}

/*! POST /api/<apikey>/sensors
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::findNewSensors(const ApiRequest &req, ApiResponse &rsp)
{
    Q_UNUSED(req);

    if (!isInNetwork())
    {
        rsp.list.append(errorToMap(ERR_NOT_CONNECTED, QLatin1String("/sensors"), QLatin1String("Not connected")));
        rsp.httpStatus = HttpStatusServiceUnavailable;
        return REQ_READY_SEND;
    }

    startFindSensors();
    {
        QVariantMap rspItem;
        QVariantMap rspItemState;
        rspItemState[QLatin1String("/sensors")] = QLatin1String("Searching for new devices");
        rspItemState[QLatin1String("/sensors/duration")] = (double)findSensorsTimeout;
        rspItem[QLatin1String("success")] = rspItemState;
        rsp.list.append(rspItem);
    }

    rsp.httpStatus = HttpStatusOk;

    return REQ_READY_SEND;
}

/*! GET /api/<apikey>/sensors/new
    \return REQ_READY_SEND
            REQ_NOT_HANDLED
 */
int DeRestPluginPrivate::getNewSensors(const ApiRequest &req, ApiResponse &rsp)
{
    Q_UNUSED(req);

    if (!findSensorResult.isEmpty() &&
        (findSensorsState == FindSensorsActive || findSensorsState == FindSensorsDone))
    {

        rsp.map = findSensorResult;
    }

    if (findSensorsState == FindSensorsActive)
    {
        rsp.map["lastscan"] = QLatin1String("active");
    }
    else if (findSensorsState == FindSensorsDone)
    {
        rsp.map["lastscan"] = lastSensorsScan;
    }
    else
    {
        rsp.map["lastscan"] = QLatin1String("none");
    }

    rsp.httpStatus = HttpStatusOk;
    return REQ_READY_SEND;
}

/*! Put all sensor parameters in a map.
    \return true - on success
            false - on error
 */
bool DeRestPluginPrivate::sensorToMap(const Sensor *sensor, QVariantMap &map, bool strictMode)
{
    if (!sensor)
    {
        return false;
    }

    QVariantMap state;
    QVariantMap config;

    for (int i = 0; i < sensor->itemCount(); i++)
    {
        const ResourceItem *item = sensor->itemForIndex(i);
        const ResourceItemDescriptor &rid = item->descriptor();

        if (rid.suffix == RConfigLat || rid.suffix == RConfigLong)
        {
            continue; //  don't return due privacy reasons
        }

        if (rid.suffix == RConfigReachable &&
            sensor->type().startsWith(QLatin1String("ZGP")))
        {
            continue; // don't provide reachable for green power devices
        }

        if (strncmp(rid.suffix, "config/", 7) == 0)
        {
            const char *key = item->descriptor().suffix + 7;
            if (rid.suffix == RConfigPending)
            {
                QVariantList pending;
                uint8_t value = item->toNumber();

                if (value & R_PENDING_DELAY)
                {
                    pending.append("delay");
                }
                if (value & R_PENDING_LEDINDICATION)
                {
                    pending.append("ledindication");
                }
                if (value & R_PENDING_SENSITIVITY)
                {
                    pending.append("sensitivity");
                }
                if (value & R_PENDING_USERTEST)
                {
                    pending.append("usertest");
                }
                config[key] = pending;
            }
            else
            {
                config[key] = item->toVariant();
            }
        }

        if (strncmp(rid.suffix, "state/", 6) == 0)
        {
            const char *key = item->descriptor().suffix + 6;

            if (rid.suffix == RStateLastUpdated && !item->lastSet().isValid())
            {
                state[key] = QLatin1String("none");
                continue;
            }

            state[key] = item->toVariant();
        }
    }

    //sensor
    map["name"] = sensor->name();

    if (strictMode)
    {
        if (sensor->manufacturer().startsWith(QLatin1String("Philips")) &&
            sensor->type().startsWith(QLatin1String("ZHA")))
        {
            QString type = sensor->type();
            type.replace(QLatin1String("ZHA"), QLatin1String("ZLL"));
            map["type"] = type;
        }
    }
    else
    {
        map["type"] = sensor->type();
    }

    if (!sensor->modelId().isEmpty())
    {
        map["modelid"] = sensor->modelId();
    }
    if (sensor->fingerPrint().endpoint != INVALID_ENDPOINT)
    {
        map["ep"] = sensor->fingerPrint().endpoint;
    }
    if (!sensor->swVersion().isEmpty() && !sensor->type().startsWith(QLatin1String("ZGP")))
    {
        map["swversion"] = sensor->swVersion();
    }
    if (sensor->mode() != Sensor::ModeNone &&
        sensor->type().endsWith(QLatin1String("Switch")))
    {
        map["mode"] = (double)sensor->mode();
    }
    if (!sensor->manufacturer().isEmpty())
    {
        map["manufacturername"] = sensor->manufacturer();
    }

    const ResourceItem *item = sensor->item(RAttrUniqueId);
    if (item)
    {
        map["uniqueid"] = item->toString();
    }
    map["state"] = state;
    map["config"] = config;

    QString etag = sensor->etag;
    etag.remove('"'); // no quotes allowed in string
    map["etag"] = etag;
    return true;
}

void DeRestPluginPrivate::handleSensorEvent(const Event &e)
{
    DBG_Assert(e.resource() == RSensors);
    DBG_Assert(e.what() != 0);

    Sensor *sensor = getSensorNodeForId(e.id());

    if (!sensor)
    {
        return;
    }
    QDateTime now = QDateTime::currentDateTime();

    // speedup sensor state check
    if (e.what() == RStatePresence &&
        sensor && sensor->durationDue.isValid())
    {
        sensorCheckFast = CHECK_SENSOR_FAST_ROUNDS;
    }

    // push sensor state updates through websocket
    if (strncmp(e.what(), "state/", 6) == 0)
    {
        ResourceItem *item = sensor->item(e.what());
        if (item)
        {
            if (sensor->lastStatePush.isValid() &&
            item->lastSet() < sensor->lastStatePush)
            {
                DBG_Printf(DBG_INFO, "discard sensor state push for %s (already pushed)\n", e.what());
                return; // already pushed
            }

            QVariantMap map;
            map["t"] = QLatin1String("event");
            map["e"] = QLatin1String("changed");
            map["r"] = QLatin1String("sensors");
            map["id"] = e.id();
            QVariantMap state;

            for (int i = 0; i < sensor->itemCount(); i++)
            {
                item = sensor->itemForIndex(i);
                const ResourceItemDescriptor &rid = item->descriptor();

                if (strncmp(rid.suffix, "state/", 6) == 0)
                {
                    const char *key = item->descriptor().suffix + 6;

                    if (item->lastSet().isValid() && (gwWebSocketNotifyAll || rid.suffix == RStateButtonEvent || (item->lastChanged().isValid() && item->lastChanged() >= sensor->lastStatePush)))
                    {
                        state[key] = item->toVariant();
                    }
                }
            }

            if (!state.isEmpty())
            {
                map["state"] = state;
                webSocketServer->broadcastTextMessage(Json::serialize(map));
                sensor->lastStatePush = now;
            }
        }
    }
    else if (strncmp(e.what(), "config/", 7) == 0)
    {
        ResourceItem *item = sensor->item(e.what());
        if (item)
        {
            if (sensor->lastConfigPush.isValid() &&
            item->lastSet() < sensor->lastConfigPush)
            {
                DBG_Printf(DBG_INFO, "discard sensor config push for %s (already pushed)\n", e.what());
                return; // already pushed
            }

            QVariantMap map;
            map["t"] = QLatin1String("event");
            map["e"] = QLatin1String("changed");
            map["r"] = QLatin1String("sensors");
            map["id"] = e.id();
            QVariantMap config;

            for (int i = 0; i < sensor->itemCount(); i++)
            {
                item = sensor->itemForIndex(i);
                const ResourceItemDescriptor &rid = item->descriptor();

                if (strncmp(rid.suffix, "config/", 7) == 0)
                {
                    const char *key = item->descriptor().suffix + 7;

                    if (rid.suffix == RConfigPending)
                    {
                        continue;
                    }
                    if (item->lastSet().isValid() && (gwWebSocketNotifyAll || (item->lastChanged().isValid() && item->lastChanged() >= sensor->lastStatePush)))
                    {
                        config[key] = item->toVariant();
                    }
                }
            }

            if (!config.isEmpty())
            {
                map["config"] = config;
                webSocketServer->broadcastTextMessage(Json::serialize(map));
                sensor->lastConfigPush = now;
            }
        }
    }
    else if (e.what() == REventAdded)
    {
        checkSensorGroup(sensor);
        checkSensorBindingsForAttributeReporting(sensor);
        checkSensorBindingsForClientClusters(sensor);

        pushSensorInfoToCore(sensor);

        QVariantMap res;
        res["name"] = sensor->name();
        findSensorResult[sensor->id()] = res;

        QVariantMap map;
        map["t"] = QLatin1String("event");
        map["e"] = QLatin1String("added");
        map["r"] = QLatin1String("sensors");

        QVariantMap smap;
        bool strictMode = false;
        sensorToMap(sensor, smap, strictMode);
        map["id"] = sensor->id();
        smap["id"] = sensor->id();
        map["sensor"] = smap;

        webSocketServer->broadcastTextMessage(Json::serialize(map));
    }
    else if (e.what() == REventDeleted)
    {
        deleteGroupsWithDeviceMembership(e.id());

        QVariantMap map;
        map["t"] = QLatin1String("event");
        map["e"] = QLatin1String("deleted");
        map["r"] = QLatin1String("sensors");

        QVariantMap smap;
        map["id"] = e.id();
        smap["id"] = e.id();
        map["sensor"] = smap;

        webSocketServer->broadcastTextMessage(Json::serialize(map));
    }
    else if (e.what() == RAttrName)
    {
        QVariantMap map;
        map["t"] = QLatin1String("event");
        map["e"] = QLatin1String("changed");
        map["r"] = QLatin1String("sensors");
        map["id"] = e.id();

        if (e.what() == RAttrName) // new attributes might be added in future
        {
            map["name"] = sensor->name();
        }
        webSocketServer->broadcastTextMessage(Json::serialize(map));
    }
    else if (e.what() == REventValidGroup)
    {
        checkOldSensorGroups(sensor);

        ResourceItem *item = sensor->item(RConfigGroup);
        DBG_Assert(item != 0);
        if (!item)
        {
            return;
        }

        Group *group = getGroupForId(item->toString());

        if (group && group->state() != Group::StateNormal)
        {
            group->setState(Group::StateNormal);
            group->setName(sensor->modelId() + QLatin1String(" ") + sensor->id());
            updateGroupEtag(group);
            queSaveDb(DB_GROUPS, DB_SHORT_SAVE_DELAY);
            DBG_Printf(DBG_INFO, "reanimate group %s\n", qPrintable(group->name()));
        }

        if (group && group->addDeviceMembership(sensor->id()))
        {
            DBG_Printf(DBG_INFO, "Attached sensor %s to group %s\n", qPrintable(sensor->id()), qPrintable(group->name()));
            queSaveDb(DB_GROUPS, DB_LONG_SAVE_DELAY);
            updateGroupEtag(group);
        }

        if (!group) // create
        {
            Group g;
            g.setAddress(item->toString().toUInt());
            g.setName(sensor->modelId() + QLatin1String(" ") + sensor->id());
            g.addDeviceMembership(sensor->id());
            groups.push_back(g);
            updateGroupEtag(&groups.back());
            queSaveDb(DB_GROUPS, DB_SHORT_SAVE_DELAY);
            checkSensorBindingsForClientClusters(sensor);
        }
    }
}

/*! Starts the search for new sensors.
 */
void DeRestPluginPrivate::startFindSensors()
{
    if (findSensorsState == FindSensorsIdle || findSensorsState == FindSensorsDone)
    {
        findSensorCandidates.clear();
        findSensorResult.clear();
        lastSensorsScan = QDateTime::currentDateTimeUtc().toString(QLatin1String("yyyy-MM-ddTHH:mm:ss"));
        QTimer::singleShot(1000, this, SLOT(findSensorsTimerFired()));
        findSensorsState = FindSensorsActive;
    }
    else
    {
        Q_ASSERT(findSensorsState == FindSensorsActive);
    }

    findSensorsTimeout = gwNetworkOpenDuration;
    gwPermitJoinResend = findSensorsTimeout;
    if (!resendPermitJoinTimer->isActive())
    {
        resendPermitJoinTimer->start(100);
    }
}

/*! Handler for find sensors active state.
 */
void DeRestPluginPrivate::findSensorsTimerFired()
{
    if (gwPermitJoinResend == 0)
    {
        if (gwPermitJoinDuration == 0)
        {
            findSensorsTimeout = 0; // done
        }
    }

    if (findSensorsTimeout > 0)
    {
        findSensorsTimeout--;
        QTimer::singleShot(1000, this, SLOT(findSensorsTimerFired()));
    }

    if (findSensorsTimeout == 0)
    {
        fastProbeAddr = deCONZ::Address();
        findSensorsState = FindSensorsDone;
    }
}

/*! Validate sensor states. */
void DeRestPluginPrivate::checkSensorStateTimerFired()
{
    if (sensors.empty())
    {
        return;
    }

    if (sensorCheckIter >= sensors.size())
    {
        sensorCheckIter = 0;
        sensorCheckFast = (sensorCheckFast > 0) ? sensorCheckFast - 1 : 0;
    }

    for (int i = 0; i < CHECK_SENSORS_MAX; i++)
    {
        if (sensorCheckIter >= sensors.size())
        {
            break;
        }

        Sensor *sensor = &sensors[sensorCheckIter];
        sensorCheckIter++;

        // automatically set presence to false, if not triggered in config.duration
        if (sensor->durationDue.isValid())
        {
            QDateTime now = QDateTime::currentDateTime();
            if (sensor->durationDue <= now)
            {
                ResourceItem *item = sensor->item(RStatePresence);
                if (item && item->toBool())
                {
                    DBG_Printf(DBG_INFO, "sensor %s (%s): disable presence\n", qPrintable(sensor->id()), qPrintable(sensor->modelId()));
                    item->setValue(false);
                    sensor->updateStateTimestamp();
                    Event e(RSensors, RStatePresence, sensor->id(), item);
                    enqueueEvent(e);
                    enqueueEvent(Event(RSensors, RStateLastUpdated, sensor->id()));
                }
                sensor->durationDue = QDateTime();
            }
            else
            {
                sensorCheckFast = CHECK_SENSOR_FAST_ROUNDS;
            }
        }
    }

    // adjust check speed if needed
    int interval = (sensorCheckFast > 0) ? CHECK_SENSOR_FAST_INTERVAL
                                         : CHECK_SENSOR_INTERVAL;
    if (interval != checkSensorsTimer->interval())
    {
        checkSensorsTimer->setInterval(interval);
    }
}

/*! Check insta mac address to model identifier.
 */
void DeRestPluginPrivate::checkInstaModelId(Sensor *sensor)
{
    if (sensor && (sensor->address().ext() & macPrefixMask) == instaMacPrefix)
    {
        if (!sensor->modelId().endsWith(QLatin1String("_1")))
        {   // extract model identifier from mac address 6th byte
            const quint64 model = (sensor->address().ext() >> 16) & 0xff;
            QString modelId;
            if      (model == 0x01) { modelId = QLatin1String("HS_4f_GJ_1"); }
            else if (model == 0x02) { modelId = QLatin1String("WS_4f_J_1"); }
            else if (model == 0x03) { modelId = QLatin1String("WS_3f_G_1"); }

            if (!modelId.isEmpty() && sensor->modelId() != modelId)
            {
                sensor->setModelId(modelId);
                sensor->setNeedSaveDatabase(true);
                updateSensorEtag(sensor);
            }
        }
    }
}

/*! Heuristic to detect the type and configuration of devices.
 */
void DeRestPluginPrivate::handleIndicationFindSensors(const deCONZ::ApsDataIndication &ind, deCONZ::ZclFrame &zclFrame)
{
//    if (findSensorsState != FindSensorsActive)
//    {
//        return;
//    }

    if (ind.profileId() == ZDP_PROFILE_ID && ind.clusterId() == ZDP_DEVICE_ANNCE_CLID)
    {
        QDataStream stream(ind.asdu());
        stream.setByteOrder(QDataStream::LittleEndian);

        quint8 seq;
        quint16 nwk;
        quint64 ext;
        quint8 macCapabilities;

        stream >> seq;
        stream >> nwk;
        stream >> ext;
        stream >> macCapabilities;

        DBG_Printf(DBG_INFO, "device announce 0x%016llX (0x%04X) mac capabilities 0x%02X\n", ext, nwk, macCapabilities);

        // filter supported devices

        // Busch-Jaeger
        if ((ext & macPrefixMask) == bjeMacPrefix)
        {
        }
        else if (macCapabilities & deCONZ::MacDeviceIsFFD)
        {
            return;
        }
        else if (macCapabilities == 0)
        {
            return;
        }

        DBG_Printf(DBG_INFO, "set fast probe address to 0x%016llX (0x%04X)\n", ext, nwk);
        fastProbeAddr.setExt(ext);
        fastProbeAddr.setNwk(nwk);
        if (!fastProbeTimer->isActive())
        {
            fastProbeTimer->start(1000);
        }

        std::vector<SensorCandidate>::iterator i = findSensorCandidates.begin();
        std::vector<SensorCandidate>::iterator end = findSensorCandidates.end();

        for (; i != end; ++i)
        {
            if (i->address.ext() == ext || i->address.nwk() == nwk)
            {
                i->address = fastProbeAddr; // nwk might have changed
                return;
            }
        }

        SensorCandidate sc;
        sc.indClusterId = ind.clusterId();
        sc.address.setExt(ext);
        sc.address.setNwk(nwk);
        sc.macCapabilities = macCapabilities;
        findSensorCandidates.push_back(sc);
        return;
    }
    else if (ind.profileId() == ZDP_PROFILE_ID)
    {

        std::vector<SensorCandidate>::iterator i = findSensorCandidates.begin();
        std::vector<SensorCandidate>::iterator end = findSensorCandidates.end();

        for (; i != end; ++i)
        {
            if (i->address.ext() == fastProbeAddr.ext())
            {
                i->indClusterId = ind.clusterId();

                if (!fastProbeTimer->isActive())
                {
                    fastProbeTimer->start(100);
                }
            }
        }
        return;
    }
    else if (ind.profileId() == ZLL_PROFILE_ID || ind.profileId() == HA_PROFILE_ID)
    {
        switch (ind.clusterId())
        {
        case ONOFF_CLUSTER_ID:
        case SCENE_CLUSTER_ID:
        case LEVEL_CLUSTER_ID:
        case VENDOR_CLUSTER_ID:
            if ((zclFrame.frameControl() & deCONZ::ZclFCClusterCommand) == 0)
            {
                return;
            }

            if (zclFrame.frameControl() & deCONZ::ZclFCDirectionServerToClient)
            {
                return;
            }
            break; // ok

        case BASIC_CLUSTER_ID:
            if (!zclFrame.isProfileWideCommand())
            {
                return;
            }

            if (zclFrame.commandId() != deCONZ::ZclReadAttributesResponseId)
            {
                return;
            }
            else if (fastProbeAddr.hasExt())
            {
                std::vector<SensorCandidate>::const_iterator i = findSensorCandidates.begin();
                std::vector<SensorCandidate>::const_iterator end = findSensorCandidates.end();

                for (; i != end; ++i)
                {
                    if (i->address.ext() == fastProbeAddr.ext())
                    {
                        if (!fastProbeTimer->isActive())
                        {
                            fastProbeTimer->start(5);
                        }
                    }
                }
            }
            break; // ok

        case IAS_ZONE_CLUSTER_ID:
            break; // ok

        default:
            return;
        }
    }
    else
    {
        return;
    }

    if (ind.dstAddressMode() != deCONZ::ApsGroupAddress && ind.dstAddressMode() != deCONZ::ApsNwkAddress)
    {
        return;
    }

    SensorCandidate *sc = 0;
    {
        std::vector<SensorCandidate>::iterator i = findSensorCandidates.begin();
        std::vector<SensorCandidate>::iterator end = findSensorCandidates.end();

        for (; i != end; ++i)
        {
            if (ind.srcAddress().hasExt() && i->address.ext() == ind.srcAddress().ext())
            {
                sc = &*i;
                break;
            }

            if (ind.srcAddress().hasNwk() && i->address.nwk() == ind.srcAddress().nwk())
            {
                sc = &*i;
                break;
            }
        }
    }

    quint8 macCapabilities = 0;
    deCONZ::Address indAddress;
    if (!sc)
    {
        Sensor *sensor = getSensorNodeForAddressAndEndpoint(ind.srcAddress(), ind.srcEndpoint());

        if (sensor)
        {
            indAddress = sensor->address();
            if (sensor->node())
            {
                macCapabilities = (int)sensor->node()->macCapabilities();
            }
        }

        if (apsCtrl && (!sensor || (macCapabilities == 0)))
        {
            int i = 0;
            const deCONZ::Node *node;

            while (apsCtrl->getNode(i, &node) == 0)
            {
                /*if (node->macCapabilities() == 0)
                {
                    // ignore
                }
                else*/ if (node->address().hasExt() && ind.srcAddress().hasExt() &&
                    ind.srcAddress().ext() == node->address().ext())
                {
                    indAddress = node->address();
                    macCapabilities = node->macCapabilities();
                    break;
                }
                else if (node->address().hasNwk() && ind.srcAddress().hasNwk() &&
                    ind.srcAddress().nwk() == node->address().nwk())
                {
                    indAddress = node->address();
                    macCapabilities = node->macCapabilities();
                    break;
                }
                i++;
            }
        }
    }

    // currently only end-devices are supported
    if (!sc && (macCapabilities == 0 || (macCapabilities & deCONZ::MacDeviceIsFFD)))
    {
        return;
    }

    if (!sc && indAddress.hasExt() && indAddress.hasNwk())
    {
        SensorCandidate sc2;
        sc2.address = indAddress;
        sc2.macCapabilities = macCapabilities;
        findSensorCandidates.push_back(sc2);
        sc = &findSensorCandidates.back();

        if (!fastProbeAddr.hasExt() && findSensorsState == FindSensorsActive)
        {
            fastProbeAddr = indAddress;
            if (!fastProbeTimer->isActive())
            {
                fastProbeTimer->start(1000);
            }
        }
    }

    if (!sc) // we need a valid candidate from device announce or cache
    {
        return;
    }

    // check for dresden elektronik devices
    if ((sc->address.ext() & macPrefixMask) == deMacPrefix)
    {
        if (sc->macCapabilities & deCONZ::MacDeviceIsFFD) // end-devices only
            return;

        if (ind.profileId() != HA_PROFILE_ID)
            return;

        SensorCommand cmd;
        cmd.cluster = ind.clusterId();
        cmd.endpoint = ind.srcEndpoint();
        cmd.dstGroup = ind.dstAddress().group();
        cmd.zclCommand = zclFrame.commandId();
        cmd.zclCommandParameter = 0;

        // filter
        if (cmd.endpoint == 0x01 && cmd.cluster == ONOFF_CLUSTER_ID)
        {
            // on: Lighting and Scene Switch left button
            DBG_Printf(DBG_INFO, "Lighting or Scene Switch left button\n");
        }
        else if (cmd.endpoint == 0x02 && cmd.cluster == ONOFF_CLUSTER_ID)
        {
            // on: Lighting Switch right button
            DBG_Printf(DBG_INFO, "Lighting Switch right button\n");
        }
        else if (cmd.endpoint == 0x01 && cmd.cluster == SCENE_CLUSTER_ID && cmd.zclCommand == 0x05
                 && zclFrame.payload().size() >= 3 && zclFrame.payload().at(2) == 0x04)
        {
            // recall scene: Scene Switch
            cmd.zclCommandParameter = zclFrame.payload()[2]; // sceneId
            DBG_Printf(DBG_INFO, "Scene Switch scene %u\n", cmd.zclCommandParameter);
        }
        else
        {
            return;
        }

        bool found = false;
        for (size_t i = 0; i < sc->rxCommands.size(); i++)
        {
            if (sc->rxCommands[i] == cmd)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            sc->rxCommands.push_back(cmd);
        }

        bool isLightingSwitch = false;
        bool isSceneSwitch = false;
        quint16 group1 = 0;
        quint16 group2 = 0;

        for (size_t i = 0; i < sc->rxCommands.size(); i++)
        {
            const SensorCommand &c = sc->rxCommands[i];
            if (c.cluster == SCENE_CLUSTER_ID && c.zclCommandParameter == 0x04 && c.endpoint == 0x01)
            {
                group1 = c.dstGroup;
                isSceneSwitch = true;
                DBG_Printf(DBG_INFO, "Scene Switch group1 0x%04X\n", group1);
                break;
            }
            else if (c.cluster == ONOFF_CLUSTER_ID && c.endpoint == 0x01)
            {
                group1 = c.dstGroup;
            }
            else if (c.cluster == ONOFF_CLUSTER_ID && c.endpoint == 0x02)
            {
                group2 = c.dstGroup;
            }

            if (!isSceneSwitch && group1 != 0 && group2 != 0)
            {
                if (group1 > group2)
                {
                    std::swap(group1, group2); // reorder
                }
                isLightingSwitch = true;
                DBG_Printf(DBG_INFO, "Lighting Switch group1 0x%04X, group2 0x%04X\n", group1, group2);
                break;
            }
        }

        Sensor *s1 = getSensorNodeForAddressAndEndpoint(ind.srcAddress(), 0x01);
        Sensor *s2 = getSensorNodeForAddressAndEndpoint(ind.srcAddress(), 0x02);

        if (isSceneSwitch || isLightingSwitch)
        {
            Sensor sensorNode;
            SensorFingerprint &fp = sensorNode.fingerPrint();
            fp.endpoint = 0x01;
            fp.deviceId = DEV_ID_ZLL_COLOR_CONTROLLER;
            fp.profileId = HA_PROFILE_ID;
            fp.inClusters.push_back(BASIC_CLUSTER_ID);
            fp.inClusters.push_back(COMMISSIONING_CLUSTER_ID);
            fp.outClusters.push_back(ONOFF_CLUSTER_ID);
            fp.outClusters.push_back(LEVEL_CLUSTER_ID);
            fp.outClusters.push_back(SCENE_CLUSTER_ID);

            sensorNode.setNode(0);
            sensorNode.address() = sc->address;
            sensorNode.setType("ZHASwitch");
            sensorNode.fingerPrint() = fp;
            sensorNode.setUniqueId(generateUniqueId(sensorNode.address().ext(), sensorNode.fingerPrint().endpoint, COMMISSIONING_CLUSTER_ID));
            sensorNode.setManufacturer(QLatin1String("dresden elektronik"));

            ResourceItem *item;
            item = sensorNode.item(RConfigOn);
            item->setValue(true);

            item = sensorNode.item(RConfigReachable);
            item->setValue(true);

            sensorNode.addItem(DataTypeInt32, RStateButtonEvent);
            sensorNode.updateStateTimestamp();

            sensorNode.setNeedSaveDatabase(true);
            updateSensorEtag(&sensorNode);

            bool update = false;

            if (!s1 && isSceneSwitch && findSensorsState == FindSensorsActive)
            {
                openDb();
                sensorNode.setId(QString::number(getFreeSensorId()));
                closeDb();
                sensorNode.setMode(Sensor::ModeScenes);
                sensorNode.setModelId(QLatin1String("Scene Switch"));
                sensorNode.setName(QString("Scene Switch %1").arg(sensorNode.id()));
                sensorNode.setNeedSaveDatabase(true);
                sensors.push_back(sensorNode);
                s1 = &sensors.back();
                updateSensorEtag(s1);
                update = true;
                Event e(RSensors, REventAdded, sensorNode.id());
                enqueueEvent(e);
            }
            else if (isLightingSwitch)
            {
                if (!s1 && findSensorsState == FindSensorsActive)
                {
                    openDb();
                    sensorNode.setId(QString::number(getFreeSensorId()));
                    closeDb();
                    sensorNode.setMode(Sensor::ModeTwoGroups);
                    sensorNode.setModelId(QLatin1String("Lighting Switch"));
                    sensorNode.setName(QString("Lighting Switch %1").arg(sensorNode.id()));
                    sensorNode.setNeedSaveDatabase(true);
                    sensors.push_back(sensorNode);
                    s1 = &sensors.back();
                    updateSensorEtag(s1);
                    update = true;
                    Event e(RSensors, REventAdded, sensorNode.id());
                    enqueueEvent(e);
                }

                if (!s2 && findSensorsState == FindSensorsActive)
                {
                    openDb();
                    sensorNode.setId(QString::number(getFreeSensorId()));
                    closeDb();
                    sensorNode.setMode(Sensor::ModeTwoGroups);
                    sensorNode.setName(QString("Lighting Switch %1").arg(sensorNode.id()));
                    sensorNode.setNeedSaveDatabase(true);
                    sensorNode.fingerPrint().endpoint = 0x02;
                    sensorNode.setUniqueId(generateUniqueId(sensorNode.address().ext(), sensorNode.fingerPrint().endpoint, COMMISSIONING_CLUSTER_ID));
                    sensors.push_back(sensorNode);
                    s2 = &sensors.back();
                    updateSensorEtag(s2);
                    update = true;
                    Event e(RSensors, REventAdded, sensorNode.id());
                    enqueueEvent(e);
                }
            }

            // check updated data
            if (s1 && s1->modelId().isEmpty())
            {
                if      (isSceneSwitch)    { s1->setModelId(QLatin1String("Scene Switch")); }
                else if (isLightingSwitch) { s1->setModelId(QLatin1String("Lighting Switch")); }
                s1->setNeedSaveDatabase(true);
                update = true;
            }

            if (s2 && s2->modelId().isEmpty())
            {
                if (isLightingSwitch) { s2->setModelId(QLatin1String("Lighting Switch")); }
                s2->setNeedSaveDatabase(true);
                update = true;
            }

            if (s1 && s1->manufacturer().isEmpty())
            {
                s1->setManufacturer(QLatin1String("dresden elektronik"));
                s1->setNeedSaveDatabase(true);
                update = true;
            }

            if (s2 && s2->manufacturer().isEmpty())
            {
                s2->setManufacturer(QLatin1String("dresden elektronik"));
                s2->setNeedSaveDatabase(true);
                update = true;
            }

            // create or update first group
            Group *g = (s1 && group1 != 0) ? getGroupForId(group1) : 0;
            if (!g && s1 && group1 != 0)
            {
                // delete older groups of this switch permanently
                deleteOldGroupOfSwitch(s1, group1);

                //create new switch group
                Group group;
                group.setAddress(group1);
                group.addDeviceMembership(s1->id());
                group.setName(QString("%1").arg(s1->name()));
                updateGroupEtag(&group);
                groups.push_back(group);
                update = true;
            }
            else if (g && s1)
            {
                if (g->state() == Group::StateDeleted)
                {
                    g->setState(Group::StateNormal);
                }

                // check for changed device memberships
                if (!g->m_deviceMemberships.empty())
                {
                    if (isLightingSwitch || isSceneSwitch) // only support one device member per group
                    {
                        if (g->m_deviceMemberships.size() > 1 || g->m_deviceMemberships.front() != s1->id())
                        {
                            g->m_deviceMemberships.clear();
                        }
                    }
                }

                if (g->addDeviceMembership(s1->id()))
                {
                    updateGroupEtag(g);
                    update = true;
                }
            }

            // create or update second group (if needed)
            g = (s2 && group2 != 0) ? getGroupForId(group2) : 0;
            if (!g && s2 && group2 != 0)
            {
                // delete older groups of this switch permanently
                deleteOldGroupOfSwitch(s2, group2);

                //create new switch group
                Group group;
                group.setAddress(group2);
                group.addDeviceMembership(s2->id());
                group.setName(QString("%1").arg(s2->name()));
                updateGroupEtag(&group);
                groups.push_back(group);
            }
            else if (g && s2)
            {
                if (g->state() == Group::StateDeleted)
                {
                    g->setState(Group::StateNormal);
                }

                // check for changed device memberships
                if (!g->m_deviceMemberships.empty())
                {
                    if (isLightingSwitch || isSceneSwitch) // only support one device member per group
                    {
                        if (g->m_deviceMemberships.size() > 1 || g->m_deviceMemberships.front() != s2->id())
                        {
                            g->m_deviceMemberships.clear();
                        }
                    }
                }

                if (g->addDeviceMembership(s2->id()))
                {
                    updateGroupEtag(g);
                    update = true;
                }
            }

            if (update)
            {
                queSaveDb(DB_GROUPS | DB_SENSORS, DB_SHORT_SAVE_DELAY);
            }
        }
    }
    else if ((sc->address.ext() & macPrefixMask) == ikeaMacPrefix)
    {
        if (sc->macCapabilities & deCONZ::MacDeviceIsFFD) // end-devices only
            return;

        if (ind.profileId() != HA_PROFILE_ID)
            return;

        // filter for remote control toggle command (large button)
        if (ind.srcEndpoint() == 0x01 && ind.clusterId() == ONOFF_CLUSTER_ID && zclFrame.commandId() == 0x02)
        {
            DBG_Printf(DBG_INFO, "ikea remote toggle button\n");

            if (findSensorsState != FindSensorsActive)
            {
                return;
            }

            Sensor *s = getSensorNodeForAddressAndEndpoint(ind.srcAddress(), ind.srcEndpoint());

            if (!s)
            {
                Sensor sensorNode;
                SensorFingerprint &fp = sensorNode.fingerPrint();
                fp.endpoint = 0x01;
                fp.deviceId = DEV_ID_ZLL_COLOR_SCENE_CONTROLLER;
                fp.profileId = HA_PROFILE_ID;
                fp.inClusters.push_back(BASIC_CLUSTER_ID);
                fp.inClusters.push_back(COMMISSIONING_CLUSTER_ID);
                fp.inClusters.push_back(POWER_CONFIGURATION_CLUSTER_ID);
                fp.outClusters.push_back(ONOFF_CLUSTER_ID);
                fp.outClusters.push_back(LEVEL_CLUSTER_ID);
                fp.outClusters.push_back(SCENE_CLUSTER_ID);

                sensorNode.setNode(0);
                sensorNode.address() = sc->address;
                sensorNode.setType("ZHASwitch");
                sensorNode.fingerPrint() = fp;
                sensorNode.setUniqueId(generateUniqueId(sensorNode.address().ext(), sensorNode.fingerPrint().endpoint, COMMISSIONING_CLUSTER_ID));
                sensorNode.setManufacturer(QLatin1String("IKEA of Sweden"));
                sensorNode.setModelId(QLatin1String("TRADFRI remote control"));
                sensorNode.setMode(Sensor::ModeColorTemperature);
                sensorNode.setNeedSaveDatabase(true);

                ResourceItem *item;
                item = sensorNode.item(RConfigOn);
                item->setValue(true);

                item = sensorNode.item(RConfigReachable);
                item->setValue(true);

                sensorNode.addItem(DataTypeInt32, RStateButtonEvent);
                sensorNode.updateStateTimestamp();

                sensorNode.setNeedSaveDatabase(true);
                updateSensorEtag(&sensorNode);

                openDb();
                sensorNode.setId(QString::number(getFreeSensorId()));
                closeDb();
                sensorNode.setName(QString("Remote control %1").arg(sensorNode.id()));
                sensors.push_back(sensorNode);
                s = &sensors.back();
                updateSensorEtag(s);
                Event e(RSensors, REventAdded, sensorNode.id());
                enqueueEvent(e);

                fastProbeAddr = sc->address;
                if (!fastProbeTimer->isActive())
                {
                    fastProbeTimer->start(100);
                }
            }
        }
        //else if (ind.srcEndpoint() == 0x01 && ind.clusterId() == SCENE_CLUSTER_ID && zclFrame.commandId() == 0x09 && zclFrame.manufacturerCode() == VENDOR_IKEA)
        else if (ind.srcEndpoint() == 0x01 && ind.clusterId() == SCENE_CLUSTER_ID  && zclFrame.manufacturerCode() == VENDOR_IKEA &&
                 zclFrame.commandId() == 0x07 && zclFrame.payload().at(0) == 0x02)
        {
            DBG_Printf(DBG_INFO, "ikea remote setup button\n");

            Sensor *s = getSensorNodeForAddressAndEndpoint(ind.srcAddress(), ind.srcEndpoint());
            if (!s)
            {
                return;
            }

            std::vector<Rule>::iterator ri = rules.begin();
            std::vector<Rule>::iterator rend = rules.end();

            QString sensorAddress(QLatin1String("/sensors/"));
            sensorAddress.append(s->id());

            bool hasRules = false;

            for (; ri != rend; ++ri)
            {
                if (ri->state() != Rule::StateNormal)
                {
                    continue;
                }

                // if (ri->status() != QLatin1String("enabled"))
                // {
                //     continue;
                // }

                std::vector<RuleCondition>::const_iterator ci = ri->conditions().begin();
                std::vector<RuleCondition>::const_iterator cend = ri->conditions().end();

                for (; ci != cend; ++ci)
                {
                    if (ci->address().startsWith(sensorAddress))
                    {
                        if (ri->name().startsWith(QLatin1String("default-ct")) && ri->owner() == QLatin1String("deCONZ"))
                        {
                            DBG_Printf(DBG_INFO, "ikea remote delete old rule %s\n", qPrintable(ri->name()));
                            ri->setState(Rule::StateDeleted);
                        }
                        else
                        {
                            hasRules = true;
                        }
                        break;
                    }
                }
            }

            if (hasRules)
            {
                DBG_Printf(DBG_INFO, "ikea remote already has custom rules\n");
            }
            else if (s->mode() == Sensor::ModeColorTemperature)
            {
                Rule r;
                RuleCondition c1;
                RuleCondition lu;
                RuleAction a;
                r.setOwner(QLatin1String("deCONZ"));
                r.setCreationtime(QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss"));
                updateEtag(r.etag);

                int ruleId = 1;
                r.setId(QString::number(ruleId));

                while (std::find_if(rules.begin(), rules.end(),
                          [&r](Rule &r2) { return r2.id() == r.id(); }) != rules.end())
                {
                    ruleId++;
                    r.setId(QString::number(ruleId));
                }

                // cw rule
                r.setName(QString("default-ct-cw-%1").arg(s->id()));

                { // state/buttonevent/4002
                    QVariantMap map;
                    map["address"] = sensorAddress + QLatin1String("/state/buttonevent");
                    map["operator"] = QLatin1String("eq");
                    map["value"] = QLatin1String("4002");
                    c1 = RuleCondition(map);
                }

                { // state/lastupdated
                    QVariantMap map;
                    map["address"] = sensorAddress + QLatin1String("/state/lastupdated");
                    map["operator"] = QLatin1String("dx");
                    lu = RuleCondition(map);
                }

                r.setConditions({c1, lu});

                a.setAddress(QString("/groups/%1/action").arg(ind.dstAddress().group()));
                a.setMethod(QLatin1String("PUT"));
                a.setBody("{\"ct_inc\": -32, \"transitiontime\":4}");
                r.setActions({a});

                rules.push_back(r);

                // ww rule
                ruleId++;
                r.setId(QString::number(ruleId));
                updateEtag(r.etag);

                while (std::find_if(rules.begin(), rules.end(),
                          [&r](Rule &r2) { return r2.id() == r.id(); }) != rules.end())
                {
                    ruleId++;
                    r.setId(QString::number(ruleId));
                }

                r.setName(QString("default-ct-ww-%1").arg(s->id()));

                { // state/buttonevent/5002
                    QVariantMap map;
                    map["address"] = sensorAddress + QLatin1String("/state/buttonevent");
                    map["operator"] = QLatin1String("eq");
                    map["value"] = QLatin1String("5002");
                    c1 = RuleCondition(map);
                }

                r.setConditions({c1, lu});

                //a.setAddress(QString("/groups/%1/action").arg(ind.dstAddress().group()));
                //a.setMethod(QLatin1String("PUT"));
                a.setBody("{\"ct_inc\": 32, \"transitiontime\":4}");
                r.setActions({a});

                rules.push_back(r);
                indexRulesTriggers();

                queSaveDb(DB_RULES, DB_SHORT_SAVE_DELAY);
            }
        }
    }
}
