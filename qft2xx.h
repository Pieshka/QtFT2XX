#ifndef QFT2XX_H
#define QFT2XX_H

#ifndef _WIN32
#error This class only supports Windows-based operating systems
#endif

#include <QIODevice>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QTimer>
#include <QSemaphore>
#include <QPointer>
#include <QList>
#include <QEventLoop>
#include <QDebug>
#include <QWinEventNotifier>

/* Although ftd2xx.h now includes windows.h automatically,
 * some older versions might not. Better safe than sorry.
 */
#include <windows.h>
#include "ftd2xx.h"

/* Latency timer value */
static constexpr uint16_t	FTDI_LATENCY        =	3;
/* FTDI fixed port name */
static constexpr const char *FTDI_NAME          =	"FTDI";
/* Default FTDI port parameters */
static constexpr int FTDI_VID					=	0x0403;
static constexpr int FTDI_PID					=	0x6001;

/* Main FT232 class
 *
 * Mainly copied from QSerialPort class with some
 * mods to fit FT232.
 * Eg.:		line property is set on a single fuction call
 *			setPort() and portName() will get/return USB VID/PID
 *			values.
 *			setBaudRate() will accept any arbitrary baudrate
 * on_FTDIreceive(): slot for receiving data
 * on_FTDImodemError(): slot for receiving modem errors
 * on_FTDIevent(): slot for receiving events
 *
 * When data is received, it will be stored on an internal
 * buffer. readyRead() signal will be emitted.
 *
 * QIODevice::read() function call will read from this buffer,
 * so call is non-blocking.
 *
 * If need a blocking call, waitForReadyRead() is implemented
 * and will return true if new data is available.
 *
 * Check bytesAvailable() if need to know how many bytes are
 * stored on buffer.
 *
 */
class FT232 : public QIODevice
{
	Q_OBJECT

public:
	enum PortError {NoError = 0x00, NotOpenError = 0x01, OverrunError = 0x02, ParityError = 0x04,
					FramingError = 0x10, BreakConditionError = 0x20, FIFOError = 0x40, ReadError = 0x80};
	Q_FLAG(PortError)
	Q_DECLARE_FLAGS(PortErrors, PortError)

    enum LineProperty {SERIAL_8N1, SERIAL_8N2, SERIAL_8E1, SERIAL_8E2,
                       SERIAL_8O1, SERIAL_8O2, SERIAL_8M1, SERIAL_8M2,
                       SERIAL_8S1, SERIAL_8S2};
	Q_ENUM(LineProperty)

	enum FlowControl {NoFlowControl, HardwareControl, SoftwareControl, DTR_DSR_FlowControl};
	Q_ENUM(FlowControl)

	enum PinoutSignal {NoSignal = 0x00, ReceivedDataSignal = 0x02, DataSetReadySignal = 0x10,
					   RingIndicatorSignal = 0x20, ClearToSendSignal = 0x80};
	Q_FLAG(PinoutSignal)
	Q_DECLARE_FLAGS(PinoutSignals, PinoutSignal)

    FT232(QObject * parent = nullptr);
    virtual ~FT232();
	bool open(QIODevice::OpenMode mode = QIODevice::ReadWrite);
	bool isSequential() const {return true;}
	qint64 bytesAvailable() const;
	bool waitForReadyRead(int msecs = 30000);

	void setPort(int VID = FTDI_VID, int PID = FTDI_PID) {usbVID = VID; usbPID = PID;}
	bool setBaudRate(qint32 baud);
	qint32 baudRate() {return FTDIbaudRate;}
	bool setLineProperty(LineProperty line);
	LineProperty lineProperty() {return  FTDIlineProperty;}
	bool setFlowControl(FlowControl flow);
	FlowControl flowControl() {return  FTDIflowControl;}
	bool setDataTerminalReady(bool set);
	bool isDataTerminalReady() {return FTDIdtr;}
	bool setRequestToSend(bool set);
	bool isRequestToSend() {return FTDIrts;}
	PinoutSignals pinoutSignals();
	PortErrors error() {return errFlag;}
	void clearError() {errFlag = NoError;}

	/* FT232 specifics QSerialPortInfo like functions */
	unsigned int chipID() {return FTDIchipID;}
	bool hasVendorIdentifier() {return true;}
	bool hasProductIdentifier() {return true;}
	int vendorIdentifier() {return usbVID;}
	int productIdentifier() {return usbPID;}
	QString portName() {return productName;}
	QString manufacturer() {return manufacturerName;}
	QString libVersion() {return libraryVersion;}
	QByteArray serialNumber() {return serialNmb;}

private:
	bool FTDIdtr, FTDIrts;
	LineProperty FTDIlineProperty;
	FlowControl FTDIflowControl;
	PortErrors errFlag;
    uint32_t FTDIbaudRate = 115200;
	int usbVID, usbPID;
	unsigned int FTDIchipID;
	QString productName;
	QByteArray serialNmb;
	QString manufacturerName;
	QString libraryVersion;

	QMutex ftdiMutex;
    FT_HANDLE ftdi;
	QByteArray FTDIreadBuffer;
	QSemaphore sem;

    QWinEventNotifier * ftdiEventNotifier;


public slots:
    void on_FTDIevent();
    void on_FTDIreceive();
    void on_FTDImodemError();
	void close();

protected:
	qint64 readData(char * data, qint64 maxSize);
	qint64 writeData(const char *data, qint64 maxSize);

signals:
	void baudRateChanged(qint32);
    void linePropertyChanged(FT232::LineProperty);
    void flowControlChanged(FT232::FlowControl);
	void dataTerminalReadyChanged(bool);
	void requestToSendChanged(bool);
	void errorOccurred();
	void readyRead();

    void connected();
};


/* Info class
 * Similar to QSerialPortInfo class
 */
class FT232Info {

public:
	static QList<FT232Info> availablePorts(int VID = FTDI_VID, int PID = FTDI_PID);
	QString portName() {return FTDI_NAME;}
	QString description() {return descr;}
	QString manufacturer() {return manuf;}
	QString serialNumber() {return serialN;}
	quint16 vendorIdentifier() {return vid;}
	quint16 productIdentifier()  {return pid;}
	bool hasVendorIdentifier() {return true;}
	bool hasProductIdentifier() {return true;}

private:
	QString descr;
	QString manuf;
	QString serialN;
	quint16 vid;
	quint16 pid;
};


#endif // QFT2XX_H

