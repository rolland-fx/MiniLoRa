#*****
#IMPORT
import utime, machine, os, gc, esp32
from machine import I2C, Pin, RTC, SPI
from gnssl76l import GNSSL76L
from sht4x import SHT4X
from micropyGPS import MicropyGPS
#*****

#*****
#CONSTANT
DEBUG = const(False)
RANGE = const(0)
RUN   = const(25000)
SHORT = const(50000)
LONG  = const(290000)
WIFI  = const(False)
SHT40 = const(True)
OPT30 = const(False)
LORAWAN = const(False)
BLE = const(False)
#*****

if LORAWAN:
#*****
    #LORA
    LORA = SPI(1, 4000000, sck=Pin(19), mosi=Pin(18), miso=Pin(20))
    LORA_CS = Pin(17, mode=Pin.OUT, value=0)
    LORA_IRQ = Pin(16, mode=Pin.IN)
    #LORA_RST = Pin(2, Pin.OUT,drive=Pin.DRIVE_0,hold=True)
    LORA_DATARATE = "SF9BW125"  # Choose from several available
    # From TTN console for device
    DEVADDR = bytearray([0x00, 0x00, 0x00, 0x00])
    NWKEY = bytearray([0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
    APP = bytearray([0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])

#*****
#GLOBAL
LORA_RST = Pin(2, Pin.OUT,drive=Pin.DRIVE_0,hold=True)
interruptCounter = 0
reboot_done = 0
WKUP = Pin(0, Pin.OUT,drive=Pin.DRIVE_0,hold=False)
i2c = I2C(0,scl=Pin(23), sda=Pin(22))
GPS = GNSSL76L(i2c)
sht = SHT4X(i2c,0x44)
my_gps = MicropyGPS()
rtc = RTC()
orange = Pin(15, Pin.OUT,drive=Pin.DRIVE_0)
blue = Pin(4, Pin.OUT,drive=Pin.DRIVE_0)
blue.on()
orange.off()
bt = Pin(7, Pin.IN)
turn = b'\x00'
#*****
#*****
#IRQ

#*****

#*****
#INIT
WKUP.on()
LORA_RST.off()
utime.sleep_ms(300)
if bt.value() == 0:
    utime.sleep_ms(1000)
orange.on()
utime.sleep_ms(1200)
GPS.low_power()
WKUP.off()
utime.sleep_ms(1500)
#*****
#if not DEBUG:
#   machine.freq(40000000)
while True:  
    data = []
    All_sentence = GPS.sentences()
    All_sentence = All_sentence[-8:]
    my_gps.fix_type = 0
    for sentence in All_sentence :
        prefix = sentence[1:6]
        my_gps.reco(prefix,sentence)      
    if DEBUG:
       del prefix
       print("test ?")
       turn_int = int.from_bytes(rtc.memory(),"big") + 1
       if ((my_gps.fix_type > 1) and (my_gps.date != [0, 0, 0]) and (my_gps.latitude != 0.00000000) and (my_gps.longitude != 0.00000000)) or (turn_int > 5):
           if (turn_int <= 5):
               blue.off()
               rtc.datetime(my_gps.time_rtc())
               f = open('data/track%s.xml' %str(my_gps.date) , 'a')
               print("position !")
               if SHT40:
                   temperature, relative_humidity = sht.measurements
               else:
                   temperature = 0
                   relative_humidity = 0
               data = my_gps.track_gpx(temperature,relative_humidity,1)
               print(data)
               f.write(data)
               f.close()
               blue.on()
               del f
           print("sleep !")
           turn_int = 0
           rtc.memory(b'\x00')
           my_gps.fix_type = 0
           GPS.deep_sleep()
           if RANGE == 0:
               utime.sleep_ms(LONG)
           elif RANGE == 1 :
               utime.sleep_ms(SHORT)
           elif RANGE == 2 :
               utime.sleep_ms(RUN)
       WKUP.on()
       print("awake")
       turn = turn_int.to_bytes(1, "big")
       rtc.memory(turn)
       if RANGE == 2:
           utime.sleep_ms(3500)
       else:
           utime.sleep_ms(7500)   
       WKUP.off()
       del data
       del All_sentence
    else:
       turn_int = int.from_bytes(rtc.memory(),"big") + 1 
       if ((my_gps.fix_type > 1) and (my_gps.date != [0, 0, 0]) and (my_gps.latitude != 0.00000000) and (my_gps.longitude != 0.00000000)) or (turn_int > 5):
           if (turn_int <= 5):    
               blue.off()
               rtc.datetime(my_gps.time_rtc())
               f = open('data/track%s.xml' %str(my_gps.date) , 'a')
               if SHT40:
                   temperature, relative_humidity = sht.measurements
               else:
                   temperature = 0
                   relative_humidity = 0
               data = my_gps.track_gpx(temperature,relative_humidity,1)
               blue.on()
               f.write(data)
               f.close()
           rtc.memory(b'\x00')
           my_gps.fix_type = 0
           WKUP.init(hold=True)
           GPS.deep_sleep()
           if RANGE == 0:
               machine.deepsleep(LONG)
           elif RANGE == 1:
               machine.deepsleep(SHORT)
           elif RANGE == 2:
               machine.deepsleep(RUN)
       WKUP.init(hold=True)
       WKUP.on()
       turn = turn_int.to_bytes(1, "big")
       rtc.memory(turn)
       if RANGE == 2:
           machine.deepsleep(3500)
       else:
           machine.deepsleep(7500)              
