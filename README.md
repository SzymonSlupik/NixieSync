The Karlsson Nixie clock (by Peter van der Jagt 1997) is very stylish. One of the first of the kind and definitely setting the trend. Unfortunately it does not hold the time after a power outage. For quite some time I was thinking of adding a time synchronization module.

Initially the idea was to use the long wave radio receiving the [DCF77](https://en.wikipedia.org/wiki/DCF77) signal. DCF77 is increasingly more problematic as the 77kHz frequency is impacted by the totality of high frequency digital power supplies that generate lots of radio noise.

The alternative is of course fully digital, relying on an Internet time source, based on the NTP protocol. Surprisingly (or not) the NTP-based module has proven to be super easy to build these days (with the help of AI).

The basic idea is this:
- At power cycle / reboot the clock starts with 00:00:00
- You advance the hours by clicking a momentary switch
- You advance the minutes by clicking another momentary switch. Advancing minutes resets seconds to :00
- So make a small WiFi module that connects to the Internet, gets the current time from a trusted NTP time server and generates the needed switch clicks to set the time.

Some additional design details:
- I decided to use the [Seeed Studio XIAO C3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) module. Very compact form factor, external WiFi antenna (the clock has thick metal enclosure), support for Arduino IDE with very simple USB-C power/programming/debug connector.
- Setup is by starting a WiFi AP on the XIAO and having all parameters on the captive portal page. This technique buys a very good UX, as basically you connect to the WiFi network and the setup page pops up automatically.
- Initially I wanted to interface to the clock via small relays (the safest option), but after some examinations of the circuit I ended up with simple MOSFET transistors.

The whole project was done with Claude AI. Besides the above design requirements I did not do much else. In particular I have NOT seen / touched the code. We (Claude and I) went through a couple of iterations that included adding support for WPA2-Enterprise (PEAP / MSCHAPv2) and improving debouncing of the reset switch. The code is 100% written by Claude.

For details see: 
- [The Arduino source code](NixieSync.INO)
- [The Claude design blueprint](NixieSync.MD)
- [The PDF PCB layout](NixieSync-Layout.PDF)
