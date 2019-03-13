This is an attempt to implement NAT in the LWIP2 for Arduino or the ESP8266
It's based on the original LWIP2 from d-a-v and uses the development of Martin-Ger from the project "esp-open-lwip" https://github.com/martin-ger/esp-open-lwip

```make install```: download, compile, install lwip2

```make download```: download lwIP-2 builder

```make clean```: clean builder only

glue and lwIP debug options are in builder/glue/gluedebug.h

MSS values are in builder/Makefile.arduino

MSS values in boards.txt are only informative

current lwip2 submodule repository: https://github.com/d-a-v/esp82xx-nonos-linklayer/tree/arduino-2.4.0
