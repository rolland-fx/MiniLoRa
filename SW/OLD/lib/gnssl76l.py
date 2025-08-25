"""
MicroPython Quectel GNSS L76-L (GPS) I2C driver
"""

import utime
from machine import I2C, Pin

class GNSSL76L:
    def __init__(self, i2c=None, address=0x10):
        if i2c is None:
            self.i2c = I2C(scl=Pin(26), sda=Pin(25))
        else:
            self.i2c = i2c

        self.address = address

    def read(self, chunksize=255):
        data = self.i2c.readfrom(self.address, chunksize)
        while data[-2:] != b"\x0a\x0a":
            utime.sleep_ms(2)
            data = data + self.i2c.readfrom(self.address, chunksize)

        return data.replace(b"\x0a", b"").replace(b"\x0d", b"\x0d\x0a")

    def low_power(self):
        data = self.i2c.writeto(self.address,b"$PMTK220,1000*1F\r\n")
        return data
    def cyclic(self):
        data = self.i2c.writeto(self.address,b"$PMTK225,2,5000,20000,0,0*1E\r\n")
        return data
    def long_cyclic(self):
        data = self.i2c.writeto(self.address,b"$PMTK225,2,8000,300000,0,0*22\r\n")
        return data  
    def end_cyclic(self):
        data = self.i2c.writeto(self.address,b"$PMTK225,0*2B\r\n")
        return data
    def sleep(self):
        data = self.i2c.writeto(self.address,b"$PMTK161,0*28\r\n")
        return data       
    def deep_sleep(self):
        data = self.i2c.writeto(self.address,b"$PMTK225,4*2F\r\n")
        return data   
    def wakeup(self):    
        data = self.i2c.writeto(self.address,b'X')
        return data   
    def sentences(self):
        return self.read().splitlines()

