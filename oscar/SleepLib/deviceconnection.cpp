/* Device Connection Class Implementation
 *
 * Copyright (c) 2020 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "deviceconnection.h"
#include <QtSerialPort/QSerialPortInfo>
#include <QFile>
#include <QBuffer>
#include <QDateTime>
#include <QDebug>


static QString hex(int i)
{
    return QString("0x") + QString::number(i, 16).toUpper();
}


// MARK: -
// MARK: XML record/playback base classes

class XmlRecord
{
public:
    XmlRecord(class QFile * file);
    XmlRecord(QString & string);
    ~XmlRecord();
    inline QXmlStreamWriter & xml() { return *m_xml; }
protected:
    QFile* m_file;  // nullptr for non-file recordings
    QXmlStreamWriter* m_xml;
    
    void prologue();
    void epilogue();
};

class XmlReplay
{
public:
    XmlReplay(class QFile * file);
    XmlReplay(QXmlStreamReader & xml);
    ~XmlReplay();
    template<class T> inline T* getNextEvent();

protected:
    void deserialize(QXmlStreamReader & xml);
    void deserializeEvents(QXmlStreamReader & xml);

    // TODO: maybe the QList should be a QHash on the timestamp?
    // Then indices would be iterators over a sorted list of keys.
    QHash<QString,QList<class XmlReplayEvent*>> m_events;
    QHash<QString,int> m_indices;

    class XmlReplayEvent* getNextEvent(const QString & type);
};

class XmlReplayEvent
{
public:
    XmlReplayEvent();
    virtual ~XmlReplayEvent() = default;
    virtual const QString & tag() const = 0;

    void record(XmlRecord* xml);
    friend QXmlStreamWriter & operator<<(QXmlStreamWriter & xml, const XmlReplayEvent & event);
    friend QXmlStreamReader & operator>>(QXmlStreamReader & xml, XmlReplayEvent & event);

    typedef XmlReplayEvent* (*FactoryMethod)();
    static bool registerClass(const QString & tag, FactoryMethod factory);
    static XmlReplayEvent* createInstance(const QString & tag);

protected:
    static QHash<QString,FactoryMethod> s_factories;

    QDateTime m_time;

    virtual void write(QXmlStreamWriter & /*xml*/) const {}
    virtual void read(QXmlStreamReader & /*xml*/) {}
};
QHash<QString,XmlReplayEvent::FactoryMethod> XmlReplayEvent::s_factories;


XmlRecord::XmlRecord(QFile* stream)
    : m_file(stream), m_xml(new QXmlStreamWriter(stream))
{
    prologue();
}

XmlRecord::XmlRecord(QString & string)
    : m_file(nullptr), m_xml(new QXmlStreamWriter(&string))
{
    prologue();
}

XmlRecord::~XmlRecord()
{
    epilogue();
    delete m_xml;
}

void XmlRecord::prologue()
{
    Q_ASSERT(m_xml);
    m_xml->setAutoFormatting(true);
    m_xml->setAutoFormattingIndent(2);
    
    m_xml->writeStartElement("xmlreplay");
    m_xml->writeStartElement("events");
}

void XmlRecord::epilogue()
{
    Q_ASSERT(m_xml);
    m_xml->writeEndElement();  // close events
    // TODO: write out any inline connections
    m_xml->writeEndElement();  // close xmlreplay
}

XmlReplay::XmlReplay(QFile* file)
{
    QXmlStreamReader xml(file);
    deserialize(xml);
}

XmlReplay::XmlReplay(QXmlStreamReader & xml)
{
    deserialize(xml);
}

XmlReplay::~XmlReplay()
{
    for (auto list : m_events.values()) {
        for (auto event : list) {
            delete event;
        }
    }
}

void XmlReplay::deserialize(QXmlStreamReader & xml)
{
    if (xml.readNextStartElement()) {
        if (xml.name() == "xmlreplay") {
            while (xml.readNextStartElement()) {
                if (xml.name() == "events") {
                    deserializeEvents(xml);
                // else TODO: inline connections
                } else {
                    qWarning() << "unexpected payload in replay XML:" << xml.name();
                    xml.skipCurrentElement();
                }
            }
        }
    }
}

void XmlReplay::deserializeEvents(QXmlStreamReader & xml)
{
    while (xml.readNextStartElement()) {
        QString name = xml.name().toString();
        XmlReplayEvent* event = XmlReplayEvent::createInstance(name);
        if (event) {
            xml >> *event;
            auto & events = m_events[name];
            events.append(event);
        } else {
            xml.skipCurrentElement();
        }
    }
}

XmlReplayEvent* XmlReplay::getNextEvent(const QString & type)
{
    XmlReplayEvent* event = nullptr;
    
    if (m_events.contains(type)) {
        auto & events = m_events[type];
        int i = m_indices[type];
        if (i < events.size()) {
            event = events[i];
            // TODO: if we're simulating the original timing, return nullptr if we haven't reached this event's time yet;
            // otherwise:
            m_indices[type] = i + 1;
        }
    }
    return event;
}

template<class T>
T* XmlReplay::getNextEvent()
{
    T* event = dynamic_cast<T*>(getNextEvent(T::TAG));
    return event;
}


// MARK: -
// MARK: XML record/playback event base class

XmlReplayEvent::XmlReplayEvent()
    : m_time(QDateTime::currentDateTime())
{
}

void XmlReplayEvent::record(XmlRecord* writer)
{
    // Do nothing if we're not recording.
    if (writer != nullptr) {
        writer->xml() << *this;
    }
}

bool XmlReplayEvent::registerClass(const QString & tag, XmlReplayEvent::FactoryMethod factory)
{
    if (s_factories.contains(tag)) {
        qWarning() << "Event class already registered for tag" << tag;
        return false;
    }
    s_factories[tag] = factory;
    return true;
}

XmlReplayEvent* XmlReplayEvent::createInstance(const QString & tag)
{
    XmlReplayEvent* event = nullptr;
    XmlReplayEvent::FactoryMethod factory = s_factories.value(tag);
    if (factory == nullptr) {
        qWarning() << "No event class registered for XML tag" << tag;
    } else {
        event = factory();
    }
    return event;
}

QXmlStreamWriter & operator<<(QXmlStreamWriter & xml, const XmlReplayEvent & event)
{
    QDateTime time = event.m_time.toOffsetFromUtc(event.m_time.offsetFromUtc());  // force display of UTC offset
#if QT_VERSION < QT_VERSION_CHECK(5,9,0)
    // TODO: Can we please deprecate support for Qt older than 5.9?
    QString timestamp = time.toString(Qt::ISODate);
#else
    QString timestamp = time.toString(Qt::ISODateWithMs);
#endif
    xml.writeStartElement(event.tag());
    xml.writeAttribute("time", timestamp);

    event.write(xml);

    xml.writeEndElement();
    return xml;
}

QXmlStreamReader & operator>>(QXmlStreamReader & xml, XmlReplayEvent & event)
{
    Q_ASSERT(xml.isStartElement() && xml.name() == event.tag());

    QDateTime time;
    if (xml.attributes().hasAttribute("time")) {
#if QT_VERSION < QT_VERSION_CHECK(5,9,0)
        // TODO: Can we please deprecate support for Qt older than 5.9?
        time = QDateTime::fromString(xml.attributes().value("time").toString(), Qt::ISODate);
#else
        time = QDateTime::fromString(xml.attributes().value("time").toString(), Qt::ISODateWithMs);
#endif
    } else {
        qWarning() << "Missing timestamp in" << xml.name() << "tag, using current time";
        time = QDateTime::currentDateTime();
    }
    
    event.read(xml);
    return xml;
}

template<typename T> QXmlStreamWriter & operator<<(QXmlStreamWriter & xml, const QList<T> & list)
{
    for (auto & item : list) {
        xml << item;
    }
    return xml;
}

template<typename T> QXmlStreamReader & operator>>(QXmlStreamReader & xml, QList<T> & list)
{
    list.clear();
    while (xml.readNextStartElement()) {
        T item;
        xml >> item;
        list.append(item);
    }
    return xml;
}

template <typename Derived>
class XmlReplayBase : public XmlReplayEvent
{
public:
    static const QString TAG;
    static const bool registered;
    virtual const QString & tag() const { return TAG; };

    static XmlReplayEvent* createInstance()
    {
        Derived* instance = new Derived();
        return static_cast<XmlReplayEvent*>(instance);
    }
};

#define REGISTER_XMLREPLAYEVENT(tag, type) \
template<> const QString XmlReplayBase<type>::TAG = tag; \
template<> const bool XmlReplayBase<type>::registered = XmlReplayEvent::registerClass(XmlReplayBase<type>::TAG, XmlReplayBase<type>::createInstance);


// MARK: -
// MARK: Device connection manager

inline DeviceConnectionManager & DeviceConnectionManager::getInstance()
{
    static DeviceConnectionManager instance;
    return instance;
}

DeviceConnectionManager::DeviceConnectionManager()
    : m_record(nullptr), m_replay(nullptr)
{
}

void DeviceConnectionManager::record(QFile* stream)
{
    if (m_record) {
        delete m_record;
    }
    if (stream) {
        m_record = new XmlRecord(stream);
    } else {
        // nullptr turns off recording
        m_record = nullptr;
    }
}

void DeviceConnectionManager::record(QString & string)
{
    if (m_record) {
        delete m_record;
    }
    m_record = new XmlRecord(string);
}

void DeviceConnectionManager::replay(const QString & string)
{
    QXmlStreamReader xml(string);
    reset();
    if (m_replay) {
        delete m_replay;
    }
    m_replay = new XmlReplay(xml);
}

void DeviceConnectionManager::replay(QFile* file)
{
    reset();
    if (m_replay) {
        delete m_replay;
    }
    if (file) {
        m_replay = new XmlReplay(file);
    } else {
        // nullptr turns off replay
        m_replay = nullptr;
    }
}


// MARK: -
// MARK: Device manager events

class GetAvailablePortsEvent : public XmlReplayBase<GetAvailablePortsEvent>
{
public:
    QList<SerialPortInfo> m_ports;

protected:
    virtual void write(QXmlStreamWriter & xml) const
    {
        xml << m_ports;
    }
    virtual void read(QXmlStreamReader & xml)
    {
        xml >> m_ports;
    }
};
REGISTER_XMLREPLAYEVENT("getAvailablePorts", GetAvailablePortsEvent);


QList<SerialPortInfo> DeviceConnectionManager::getAvailablePorts()
{
    GetAvailablePortsEvent event;

    if (!m_replay) {
        for (auto & info : QSerialPortInfo::availablePorts()) {
            event.m_ports.append(SerialPortInfo(info));
        }
    } else {
        auto replayEvent = m_replay->getNextEvent<GetAvailablePortsEvent>();
        if (replayEvent) {
            event.m_ports = replayEvent->m_ports;
        } else {
            // If there are no replay events available, reuse the most recent state.
            event.m_ports = m_serialPorts;
        }
    }
    m_serialPorts = event.m_ports;

    event.record(m_record);
    return event.m_ports;
}


// TODO: Once we start recording/replaying connections, we'll need to include a version number, so that
// if we ever have to change the download code, the older replays will still work as expected.


// MARK: -
// MARK: Serial port info

SerialPortInfo::SerialPortInfo(const QSerialPortInfo & other)
{
    if (other.isNull() == false) {
        m_info["portName"] = other.portName();
        m_info["systemLocation"] = other.systemLocation();
        m_info["description"] = other.description();
        m_info["manufacturer"] = other.manufacturer();
        m_info["serialNumber"] = other.serialNumber();
        if (other.hasVendorIdentifier()) {
            m_info["vendorIdentifier"] = other.vendorIdentifier();
        }
        if (other.hasProductIdentifier()) {
            m_info["productIdentifier"] = other.productIdentifier();
        }
    }
}

SerialPortInfo::SerialPortInfo(const SerialPortInfo & other)
    : m_info(other.m_info)
{
}

SerialPortInfo::SerialPortInfo(const QString & data)
{
    QXmlStreamReader xml(data);
    xml.readNextStartElement();
    xml >> *this;
}

SerialPortInfo::SerialPortInfo()
{
}

// TODO: This is a temporary wrapper until we begin refactoring.
QList<SerialPortInfo> SerialPortInfo::availablePorts()
{
    return DeviceConnectionManager::getInstance().getAvailablePorts();
}

QXmlStreamWriter & operator<<(QXmlStreamWriter & xml, const SerialPortInfo & info)
{
    xml.writeStartElement("serial");
    if (info.isNull() == false) {
        xml.writeAttribute("portName", info.portName());
        xml.writeAttribute("systemLocation", info.systemLocation());
        xml.writeAttribute("description", info.description());
        xml.writeAttribute("manufacturer", info.manufacturer());
        xml.writeAttribute("serialNumber", info.serialNumber());
        if (info.hasVendorIdentifier()) {
            xml.writeAttribute("vendorIdentifier", hex(info.vendorIdentifier()));
        }
        if (info.hasProductIdentifier()) {
            xml.writeAttribute("productIdentifier", hex(info.productIdentifier()));
        }
    }
    xml.writeEndElement();
    return xml;
}

QXmlStreamReader & operator>>(QXmlStreamReader & xml, SerialPortInfo & info)
{
    if (xml.atEnd() == false && xml.isStartElement() && xml.name() == "serial") {
        for (auto & attribute : xml.attributes()) {
            QString name = attribute.name().toString();
            QString value = attribute.value().toString();
            if (name == "vendorIdentifier" || name == "productIdentifier") {
                bool ok;
                quint16 id = value.toUInt(&ok, 0);
                if (ok) {
                    info.m_info[name] = id;
                } else {
                    qWarning() << "invalid" << name << "value" << value;
                }
            } else {
                info.m_info[name] = value;
            }
        }
    } else {
        qWarning() << "no <serial> tag";
    }
    xml.skipCurrentElement();
    return xml;
}

SerialPortInfo::operator QString() const
{
    QString out;
    QXmlStreamWriter xml(&out);
    xml << *this;
    return out;
}

bool SerialPortInfo::operator==(const SerialPortInfo & other) const
{
    return m_info == other.m_info;
}


// MARK: -
// MARK: Serial port connection

// TODO: log these to XML

class SetValueEvent
{
public:
    SetValueEvent(const QString & name, int value)
    {
        set(name, value);
    }
    void set(const QString & name, int value)
    {
        m_values[name] = value;
        m_keys.append(name);
    }
    inline bool ok() const { return m_values.contains("error") == false; }
    operator QString() const;

protected:
    QHash<QString,int> m_values;
    QList<QString> m_keys;
    friend QXmlStreamWriter & operator<<(QXmlStreamWriter & xml, const SetValueEvent & event);
};

SetValueEvent::operator QString() const
{
    QString out;
    QXmlStreamWriter xml(&out);
    xml << *this;
    return out;
}

QXmlStreamWriter & operator<<(QXmlStreamWriter & xml, const SetValueEvent & event)
{
    xml.writeStartElement("set");
    for (auto key : event.m_keys) {
        xml.writeAttribute(key, QString::number(event.m_values[key]));
    }
    xml.writeEndElement();
    return xml;
}


SerialPort::SerialPort()
{
    connect(&m_port, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
}

SerialPort::~SerialPort()
{
    disconnect(&m_port, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
}

void SerialPort::setPortName(const QString &name)
{
    qDebug() << "<setPortName>";
    return m_port.setPortName(name);
}

bool SerialPort::open(QIODevice::OpenMode mode)
{
    qDebug() << "<open>";
    return m_port.open(mode);
}

bool SerialPort::setBaudRate(qint32 baudRate, QSerialPort::Directions directions)
{
    SetValueEvent event("baudRate", baudRate);
    event.set("directions", directions);

    bool ok = m_port.setBaudRate(baudRate, directions);
    if (!ok) {
        QSerialPort::SerialPortError error = m_port.error();
        event.set("error", error);
    }
    qDebug().noquote() << event;

    return event.ok();
}
// TODO: <set time="FOO" name="baudrate" value="19200"/>

bool SerialPort::setDataBits(QSerialPort::DataBits dataBits)
{
    SetValueEvent event("setDataBits", dataBits);

    bool ok = m_port.setDataBits(dataBits);
    if (!ok) {
        QSerialPort::SerialPortError error = m_port.error();
        event.set("error", error);
    }
    qDebug().noquote() << event;

    return event.ok();
}

bool SerialPort::setParity(QSerialPort::Parity parity)
{
    SetValueEvent event("setParity", parity);

    bool ok = m_port.setParity(parity);
    if (!ok) {
        QSerialPort::SerialPortError error = m_port.error();
        event.set("error", error);
    }
    qDebug().noquote() << event;

    return event.ok();
}

bool SerialPort::setStopBits(QSerialPort::StopBits stopBits)
{
    SetValueEvent event("setStopBits", stopBits);

    bool ok = m_port.setStopBits(stopBits);
    if (!ok) {
        QSerialPort::SerialPortError error = m_port.error();
        event.set("error", error);
    }
    qDebug().noquote() << event;

    return event.ok();
}

bool SerialPort::setFlowControl(QSerialPort::FlowControl flowControl)
{
    SetValueEvent event("setFlowControl", flowControl);

    bool ok = m_port.setFlowControl(flowControl);
    if (!ok) {
        QSerialPort::SerialPortError error = m_port.error();
        event.set("error", error);
    }
    qDebug().noquote() << event;

    return event.ok();
}

bool SerialPort::clear(QSerialPort::Directions directions)
{
    qDebug() << "<clear>";
    return m_port.clear(directions);
}

qint64 SerialPort::bytesAvailable() const
{
    qDebug() << "<bytesAvailable>";
    return m_port.bytesAvailable();
}

qint64 SerialPort::read(char *data, qint64 maxSize)
{
    qDebug() << "<rx>";
    return m_port.read(data, maxSize);
}

qint64 SerialPort::write(const char *data, qint64 maxSize)
{
    qDebug() << "<tx>";
    return m_port.write(data, maxSize);
}

bool SerialPort::flush()
{
    qDebug() << "<flush>";
    return m_port.flush();
}

void SerialPort::close()
{
    qDebug() << "<close>";
    return m_port.close();
}

void SerialPort::onReadyRead()
{
    qDebug() << "<readyRead>";
    emit readyRead();
}

