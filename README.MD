# Qt FT2XX - A wrapper around FTD2XX for Qt
###  The Issue
Victor Preatoni [made](https://github.com/vpreatoni/QtFT232) some time ago, a QIODevice-compatible wrapper around Intra2net libFTDI - an opensource, portable alternative for the FTDI's proprietary DLL. It's very good and I highly recommend using it when developing applications for your own FTDI-oriented devices. But what if you want write an application for a third-party device? Or you are a company which policies requires using `officialy supported` libraries and drivers (in this case FTDI-provided resources)? Well in those cases using libFTDI might not be an option for you, because it forces you to replace the FTDI driver with libUSB one (on Windows, on Linux there are other quirky issues like unloading the kernel default driver). Sticking with the FTDI driver and FTD2XX is the only way to go. But `in terms of Qt classes, we have no Qt classes`.

###  The solution
The solution is here! I've ported Victor's class for the official FTD2XX library, so now you can use your favorite FTDI chip in Qt with ease and without any fussing around the drivers. The drawback is - I've made it only for Windows-based operating systems.

Victor's class used a polling mechanism for obtaining data and modem status from the FTDI chip. My version completely ditches this in favor of the event notification system supported by the official library. This is the only thing using Windows-specific functions, so you might be able to easily bring Linux support by adding Linux's way of handling events.

This class is copying some of the QSerialPort behavior (just like the original one), so you may use it with the QIODevice base class in a proxy pattern situation (like providing multiple ways to connect to a device)

### License
Because it is based around GPL3-licensed project, it is available only under the GPL3 terms. (But if it becomes possible, I am giving green light to use it under the MIT License)