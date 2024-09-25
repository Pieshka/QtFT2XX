/* FT2XX wrapper for Qt
 *
 * 29/03/2020	First version
 * 21/08/2024   Adapted for the official FTDI driver
 *
 *
 * Author: Victor Preatoni
 *         Pieszka
 *
 */

#include "qft2xx.h"
/* Class constructor
 */
FT232::FT232(QObject *parent)
	: QIODevice (parent)
{

}

/* Releases FT_HANDLE and deletes ftdiEventNotifier
 */
FT232::~FT232()
{
    FT_Close(ftdi);
    delete ftdiEventNotifier;
}

/* Open FT232 and sets basic parameters:
 * baudRate, latency and chunksize.
 * It will also try to read some FT232 information,
 * like chipID, vendor name and product name.
 *
 * Event notification is enabled here too
 */
bool FT232::open(QIODevice::OpenMode mode)
{
    FT_STATUS ret;
    FT_DEVICE_LIST_INFO_NODE *devinfo;
    DWORD numDevs;
    int deviceId = -1;

    ret = FT_CreateDeviceInfoList(&numDevs);
    if(ret != FT_OK)
    {
        setErrorString(tr("an error occured while enumerating devices"));
        return false;
    }

    devinfo = (FT_DEVICE_LIST_INFO_NODE*)malloc(sizeof(FT_DEVICE_LIST_INFO_NODE)*numDevs);
    ret = FT_GetDeviceInfoList(devinfo, &numDevs);
    if(ret != FT_OK)
    {
        setErrorString(tr("an error occured while obtaining device info list"));
        return false;
    }

    /* Find device id of the correct FTDI chip */
    for(uint i = 0; i < numDevs; i++)
    {
        if(devinfo[i].ID == (ulong)((ulong)usbVID*0x10000 + (ulong)usbPID))
        {
            deviceId = i;
            break;
        }
    }
    free(devinfo);

    if(deviceId == -1)
    {
        setErrorString(tr("no compatible devices found"));
        return false;
    }

    ret = FT_Open(deviceId,&ftdi);
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while opening the device"));
		return false;
	}

    ret = FT_SetBaudRate(ftdi, FTDIbaudRate);
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the baudrate"));
		close();
		return false;
	}

    ret = FT_SetLatencyTimer(ftdi, FTDI_LATENCY);
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the latency timer"));
        close();
        return false;
    }

    ret = FT_SetTimeouts(ftdi,5000,2000);
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the timeouts"));
        close();
        return false;
    }

	/* Now read some FT232R specifics
	 */
    FT_PROGRAM_DATA ftData;
    char ManufacturerBuf[32];
    char ManufacturerIdBuf[16];
    char DescriptionBuf[64];
    char SerialNumberBuf[16];

    ftData.Signature1 = 0x00000000;
    ftData.Signature2 = 0xffffffff;
    ftData.Version = 0x00000002;    // EEPROM structure with FT232R extensions
    ftData.Manufacturer = ManufacturerBuf;
    ftData.ManufacturerId = ManufacturerIdBuf;
    ftData.Description = DescriptionBuf;
    ftData.SerialNumber = SerialNumberBuf;

    ret = FT_EE_Read(ftdi, &ftData);
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while reading the EEPROM"));
        close();
        return false;
    }

    productName = QString(ftData.Description);
    serialNmb = QString(ftData.SerialNumber).toUpper().toUtf8();
    manufacturerName = QString(ftData.Manufacturer);

    DWORD libVer;
    ret = FT_GetLibraryVersion(&libVer);
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while getting the FTD2XX library version"));
        close();
        return false;
    }
    uchar major = libVer & 0xFF0000;
    uchar minor = libVer & 0xFF00;
    uchar build = libVer & 0xFF;
    libraryVersion = QStringLiteral("%1.%2.%3").arg(major, 1, 10, QLatin1Char('0')).arg(minor, 2, 10, QLatin1Char('0')).arg(build, 2, 10, QLatin1Char('0'));

	/* Clear buffers */
    FT_Purge(ftdi, FT_PURGE_RX | FT_PURGE_TX);

	/* Notify parent class we are open*/
	QIODevice::open(mode);

    /* Create event handler and QWinEventNotifier for the receive and modem status events */
    HANDLE hEvent = CreateEvent(NULL, false, false, NULL);
    ftdiEventNotifier = new QWinEventNotifier(hEvent);

    ret = FT_SetEventNotification(ftdi, FT_EVENT_RXCHAR | FT_EVENT_MODEM_STATUS, hEvent);
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the event notification"));
        close();
        return false;
    }

    /* Enable ftdiEventNotifier */
    ftdiEventNotifier->setEnabled(true);

    /* Connect event notifications */
    connect(ftdiEventNotifier, &QWinEventNotifier::activated, this, &FT232::on_FTDIevent);

    emit connected();

	return true;
}


/* Close port
 */
void FT232::close()
{
	/* Close FTDI
	 *
     * Use mutex here to avoid issues
     * when closing the FTDI handle
	 */
	ftdiMutex.lock();
    FT_Close(ftdi);
	ftdiMutex.unlock();

	setOpenMode(NotOpen);

	/* Signal we are closing */
	emit aboutToClose();
}


/* This function is called by QIODevice::read()
 */
qint64 FT232::readData(char *data, qint64 maxSize)
{
	/* Check bounds */
	qint64 n = qMin(maxSize, (qint64)FTDIreadBuffer.size());

	/* No data, return immediately */
	if (!n)
		return 0;

	/* Try to catch n bytes */
	if (!sem.tryAcquire(n))
		return 0;

	/* Copy data to QIODevice provided buffer */
	memcpy(data, FTDIreadBuffer.data(), n);

	/* Erase data from my buffer */
	FTDIreadBuffer.remove(0, n);

	return n;
}

/* This function is called by QIODevice::write()
 */
qint64 FT232::writeData(const char *data, qint64 maxSize)
{
    FT_STATUS ret;
    DWORD _bytesWritten;
	/* Write to FTDI
	 *
	 * Use mutex here to avoid writing
     * while the event handler is reading
	 */

	ftdiMutex.lock();
    ret = FT_Write(ftdi, (char *)data, maxSize, &_bytesWritten);
	ftdiMutex.unlock();

	/* If error, setErrorString */
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while writing to the port"));
		return -1;
	}

	return ret;
}

bool FT232::setBaudRate(qint32 baud)
{
    FT_STATUS ret;
	FTDIbaudRate = baud;

    /* If we are not open, just return */
    if(!isOpen()) return true;

	/* Change baud rate
	 *
	 * Use mutex here to avoid changing
     * while the event handler is reading
	 */
	ftdiMutex.lock();
    ret = FT_SetBaudRate(ftdi, FTDIbaudRate);
	ftdiMutex.unlock();

	/* If error, setErrorString */
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the baudrate"));
        close();
        return false;
    }

	/* Signal baudrate has changed */
	emit baudRateChanged(baud);

	return true;
}


bool FT232::setLineProperty(LineProperty line)
{
    FT_STATUS ret;
    uchar bits;
    uchar sbit;
    uchar parity;

    bits = FT_BITS_8;
	FTDIlineProperty = line;

	switch (line) {
    case SERIAL_8N1: parity = FT_PARITY_NONE; sbit = FT_STOP_BITS_1; break;
    case SERIAL_8N2: parity = FT_PARITY_NONE; sbit = FT_STOP_BITS_2; break;
    case SERIAL_8E1: parity = FT_PARITY_EVEN; sbit = FT_STOP_BITS_1; break;
    case SERIAL_8E2: parity = FT_PARITY_EVEN; sbit = FT_STOP_BITS_2; break;
    case SERIAL_8O1: parity = FT_PARITY_ODD; sbit = FT_STOP_BITS_1; break;
    case SERIAL_8O2: parity = FT_PARITY_ODD; sbit = FT_STOP_BITS_2; break;
    case SERIAL_8M1: parity = FT_PARITY_MARK; sbit = FT_STOP_BITS_1; break;
    case SERIAL_8M2: parity = FT_PARITY_MARK; sbit = FT_STOP_BITS_2; break;
    case SERIAL_8S1: parity = FT_PARITY_SPACE; sbit = FT_STOP_BITS_1; break;
    case SERIAL_8S2: parity = FT_PARITY_SPACE; sbit = FT_STOP_BITS_2; break;
    default: parity = FT_PARITY_NONE; sbit = FT_STOP_BITS_1; break;
	}

	/* Change line properties
	 *
	 * Use mutex here to avoid changing
     * while the event handler is reading
	 */
	ftdiMutex.lock();
    ret = FT_SetDataCharacteristics(ftdi,bits,sbit,parity);
	ftdiMutex.unlock();

	/* If error, setErrorString */
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the data characteristics"));
		return false;
	}

	/* Signal line property has changed */
	emit linePropertyChanged(line);

	return true;
}


bool FT232::setFlowControl(FlowControl flow)
{
    FT_STATUS ret;
	int flowctrl;

	switch (flow) {
    case NoFlowControl: flowctrl = FT_FLOW_NONE; break;
    case HardwareControl: flowctrl = FT_FLOW_RTS_CTS; break;
    case SoftwareControl: flowctrl = FT_FLOW_XON_XOFF; break;
    case DTR_DSR_FlowControl: flowctrl = FT_FLOW_DTR_DSR; break;
    default: flowctrl = FT_FLOW_NONE;
	}

	/* Change flow control
	 *
	 * Use mutex here to avoid changing
     * while the event handler is reading
	 */
	ftdiMutex.lock();
    ret = FT_SetFlowControl(ftdi, flowctrl, 0x11, 0x13);
	ftdiMutex.unlock();

	/* If error, setErrorString */
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the flow control"));
        return false;
    }

	FTDIflowControl = flow;
	/* Signal flow control has changed */
	emit flowControlChanged(flow);

	return true;
}


bool FT232::setDataTerminalReady(bool set)
{
    FT_STATUS ret;

	/* DTR can only be set while port
	 * is open
	 */
	if (!isOpen())
		return false;

	/* Set DTR
	 *
	 * Use mutex here to avoid changing
     * while the event handler is reading
	 */
	ftdiMutex.lock();
    if(set)
        ret = FT_SetDtr(ftdi);
    else
        ret = FT_ClrDtr(ftdi);
	ftdiMutex.unlock();

	/* If error, setErrorString */
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the DTR"));
        return false;
    }

	FTDIdtr = set;
	/* Signal DTR has changed */
	emit dataTerminalReadyChanged(set);

	return true;
}


bool FT232::setRequestToSend(bool set)
{
    FT_STATUS ret;

	/* RTS can only be set while port
	 * is open
	 */
	if (!isOpen())
		return false;

	/* Set RTS
	 *
	 * Use mutex here to avoid changing
     * while the event handler is reading
	 */
	ftdiMutex.lock();
    if(set)
        ret = FT_SetRts(ftdi);
    else
        ret = FT_ClrRts(ftdi);
	ftdiMutex.unlock();

	/* If error, setErrorString */
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while setting the RTS"));
        return false;
    }

	FTDIrts = set;
	/* Signal RTS has changed */
	emit requestToSendChanged(set);

	return true;
}


/* Parses first byte of modemStatus and returns
 * active data lines
 */
FT232::PinoutSignals FT232::pinoutSignals()
{
	/* Line status bit field
	*	B0..B3 - must be 0
	*	B4 Clear to send (CTS) 0 = inactive; 1 = active
	*	B5 Data set ready (DTS) 0 = inactive; 1 = active
	*	B6 Ring indicator (RI) 0 = inactive; 1 = active
	*	B7 Receive line signal detect (RLSD) 0 = inactive; 1 = active
	*/

    FT_STATUS ret;

	/* Signals can only be
	 * read while port is open
	 */
	if (!isOpen())
		return NoSignal;

    DWORD modemStatus;
	/* Read modem status
	 *
	 * Use mutex here to avoid reading status
     * while the event handler is reading
	 */
	ftdiMutex.lock();
    ret = FT_GetModemStatus(ftdi, &modemStatus);
	ftdiMutex.unlock();

	/* If error, setErrorString */
    if (ret != FT_OK) {
        setErrorString(tr("an error occured while reading the modem status"));
		return NoSignal;
	}

    FT232::PinoutSignals retval = NoSignal;

	if (modemStatus & 0x0080)
		retval |= ReceivedDataSignal;

	if (modemStatus & 0x0040)
		retval |= RingIndicatorSignal;

	if (modemStatus & 0x0020)
		retval |= DataSetReadySignal;

	if (modemStatus & 0x0010)
		retval |= ClearToSendSignal;

    return retval;
}

void FT232::on_FTDIevent()
{
    DWORD EventDWord;
    DWORD RxBytes;
    DWORD TxBytes;
    FT_STATUS ret;

    /* Read device status
     *
     * Use mutex here to avoid reading status
     * while the event handler is reading
     */
    ftdiMutex.lock();
    ret = FT_GetStatus(ftdi, &RxBytes, &TxBytes, &EventDWord);
    ftdiMutex.unlock();
    /* Very serious error, stop everything */
    if (ret != FT_OK) {
        /* setErrorString */
        setErrorString(tr("an error occured while reading the device status"));
        if (!isOpen())
            errFlag = NotOpenError;
        else
            errFlag = ReadError;
        emit errorOccurred();

        return;
    }

    /* If the event is modem error fire the on_FTDImodemError() function
     * otherwise if it is a RXCHAR event fire the on_FTDIreceive() function
     *
     * Ignores all unknown events
     */
    if(EventDWord & FT_EVENT_MODEM_STATUS)
        on_FTDImodemError();
    else if (EventDWord & FT_EVENT_RXCHAR)
        on_FTDIreceive();

}


/* Parses second byte of modemStatus and returns
 * active error flags
 */
void FT232::on_FTDImodemError()
{
    /* modemStatus Bit field  (*):serious errors
    * B8	Data Ready (DR)
    * B9*	Overrun Error (OE)
    * B10*	Parity Error (PE)
    * B11*	Framing Error (FE)
    * B12	Break Interrupt (BI)
    * B13	Transmitter Holding Register (THRE)
    * B14	Transmitter Empty (TEMT)
    * B15*	Error in RCVR FIFO */
    FT_STATUS ret;
    DWORD modemStatus;

    /* Get mutex before reading */
    ftdiMutex.lock();
    ret = FT_GetModemStatus(ftdi, &modemStatus);
    ftdiMutex.unlock();

    /* Very serious error, stop everything */
    if (ret != FT_OK) {
        /* setErrorString */
        setErrorString(tr("an error occured while reading the device status"));
        if (!isOpen())
            errFlag = NotOpenError;
        else
            errFlag = ReadError;
        emit errorOccurred();

        return;
    }

    /* Mask out serious errors */
    if (modemStatus & 0b1000111000000000) {
        /* Get mutex before flushing buffers */
        ftdiMutex.lock();
        FT_Purge(ftdi, FT_PURGE_RX | FT_PURGE_TX);
        ftdiMutex.unlock();

        return;
    }

    FT232::PortErrors retval = NoError;

	if (modemStatus & 0x8000)
		retval |= FIFOError;

	if (modemStatus & 0x1000)
		retval |= BreakConditionError;

	if (modemStatus & 0x0800)
		retval |= FramingError;

	if (modemStatus & 0x0400)
		retval |= ParityError;

	if (modemStatus & 0x0200)
		retval |= OverrunError;

	errFlag = retval;

	//Modem errors are quite frequent, so not emited
//	emit errorOccurred();
}


/* Returns the number of bytes that are available for reading.
 * Subclasses that reimplement this function must call
 * the base implementation in order to include the size of the buffer of QIODevice
 */
qint64 FT232::bytesAvailable() const
{
	qint64 my_size = FTDIreadBuffer.size();
	qint64 builtin_size = QIODevice::bytesAvailable();

	return (my_size + builtin_size);
}

/* Write received data to the end of internal
 * intermediate buffer
 */
void FT232::on_FTDIreceive()
{
    DWORD bytesReturned = 0;
    DWORD bytesAvailable = 0;
    char *buff;
    FT_STATUS ret;

    /* Get mutex before reading */
    ftdiMutex.lock();
    ret = FT_GetQueueStatus(ftdi,&bytesAvailable);
    if(bytesAvailable > 0)
    {
        /* If any bytes are available, create accordingly a buffer and read them all */
        buff = new char [bytesAvailable];
        ret = FT_Read(ftdi, buff, bytesAvailable, &bytesReturned);
    }
    ftdiMutex.unlock();

    /* FTDI buffer overflow */
    if (bytesAvailable > 0 && ret == FT_IO_ERROR) {
        /* setErrorString */
        setErrorString(tr("an IO error occured"));
        if (!isOpen())
            errFlag = NotOpenError;
        else
            errFlag = ReadError;
        emit errorOccurred();

        return;
    }

    /* Very serious error, stop thread */
    if (bytesAvailable > 0 && ret != FT_OK) {
        /* setErrorString */
        setErrorString(tr("an error occured while reading bytes from the device"));
        if (!isOpen())
            errFlag = NotOpenError;
        else
            errFlag = ReadError;
        emit errorOccurred();

        return;
    }

    /* Read OK, emit data */
    if (bytesAvailable > 0)
    {
        QByteArray data = QByteArray(buff,bytesReturned);
        /* Append new data */
        FTDIreadBuffer.append(data);

        /* Delete the buffer */
        delete buff;
        /* Release n bytes */
        sem.release(data.size());

        /* Emit signals */
        emit readyRead();
        emit QIODevice::readyRead();
    }
}

/* Timeout blocking function that waits
 * for bytes available on buffer to read
 */
bool FT232::waitForReadyRead(int msecs)
{
	if (!isOpen())
		return false;

	/* Create a timer and an
	 * Event Loop
	 */
	QEventLoop loop;
	QTimer timer;
	timer.setSingleShot(true);
    connect(this, &FT232::readyRead, &loop, &QEventLoop::quit);
	connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

	/* Trigger timer and run
	 * event loop
	 */
	timer.start(msecs);
	loop.exec();

	/* If timer timed out, means
	 * we had a timeout!
	 */
	if (!timer.isActive()) {
		setErrorString(tr("Read timeout"));
		return false;
	}

	/* Otherwise, we exit event loop because
	 * we catch a readyRead signal
	 */
	return true;
}

/* Static function that finds all available FTDI
 * ports and returns a list to FT232Info items
 */
QList<FT232Info> FT232Info::availablePorts(int VID, int PID)
{
	QList<FT232Info> list;
	list.clear();
	FT232Info device;

    FT_STATUS ret;
    FT_DEVICE_LIST_INFO_NODE *devinfo;
    DWORD numDevs;

    /* Return empty list if cannot create the device info list */
    ret = FT_CreateDeviceInfoList(&numDevs);
    if(ret != FT_OK)
    {
        return list;
    }

    /* Return empty list if cannot obtain the device list */
    devinfo = (FT_DEVICE_LIST_INFO_NODE*)malloc(sizeof(FT_DEVICE_LIST_INFO_NODE)*numDevs);
    ret = FT_GetDeviceInfoList(devinfo, &numDevs);
    if(ret != FT_OK)
    {
        return list;
    }
    for(uint i = 0; i < numDevs; i++)
    {
        if(devinfo[i].ID == (ulong)((ulong)VID*0x10000 + (ulong)PID))
        {
            FT_HANDLE ft;
            ret = FT_Open(i,&ft);
            if (ret != FT_OK) {
                return list;
            }

            FT_PROGRAM_DATA ftData;
            char ManufacturerBuf[32];
            char ManufacturerIdBuf[16];
            char DescriptionBuf[64];
            char SerialNumberBuf[16];

            ftData.Signature1 = 0x00000000;
            ftData.Signature2 = 0xffffffff;
            ftData.Version = 0x00000002; // EEPROM structure with FT232R extensions
            ftData.Manufacturer = ManufacturerBuf;
            ftData.ManufacturerId = ManufacturerIdBuf;
            ftData.Description = DescriptionBuf;
            ftData.SerialNumber = SerialNumberBuf;

            ret = FT_EE_Read(ft, &ftData);
            if (ret != FT_OK) {
                return list;
            }

            device.manuf = QString(ftData.Manufacturer);
            device.descr = QString(ftData.Description);
            device.serialN = QString(ftData.SerialNumber);
            device.vid = ftData.VendorId;
            device.pid = ftData.ProductId;

            /* Append data to list */
            list.append(device);

            /* Remember always to close the port after reading from the EEPROM */
            FT_Close(ft);
        }
    }

	/* Release resources */
    free(devinfo);

	return list;
}
