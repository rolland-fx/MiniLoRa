# SPDX-FileCopyrightText: Copyright (c) 2023 Jose D. Montoya
#
# SPDX-License-Identifier: MIT
"""
`sht4x`
================================================================================

MicroPython Driver fot the Sensirion Temperature and Humidity SHT40, SHT41 and SHT45 Sensor


* Author: Jose D. Montoya


"""

import time
import struct
from micropython import const

try:
    from typing import Tuple
except ImportError:
    pass

_RESET = const(0x94)

HIGH_PRECISION = const(0)
MEDIUM_PRECISION = const(1)
LOW_PRECISION = const(2)
temperature_precision_options = (HIGH_PRECISION, MEDIUM_PRECISION, LOW_PRECISION)
temperature_precision_values = {
    HIGH_PRECISION: const(0xFD),
    MEDIUM_PRECISION: const(0xF6),
    LOW_PRECISION: const(0xE0),
}

HEATER200mW = const(0)
HEATER110mW = const(1)
HEATER20mW = const(2)
heater_power_values = (HEATER200mW, HEATER110mW, HEATER20mW)

TEMP_1 = const(0)
TEMP_0_1 = const(1)
heat_time_values = (TEMP_1, TEMP_0_1)

wat_config = {
    HEATER200mW: (0x39, 0x32),
    HEATER110mW: (0x2F, 0x24),
    HEATER20mW: (0x1E, 0x15),
}


class SHT4X:

    def __init__(self, i2c, address: int = 0x44) -> None:
        self._i2c = i2c
        self._address = address
        self._data = bytearray(6)

        self._command = 0xFD
        self._temperature_precision = HIGH_PRECISION
        self._heater_power = HEATER20mW
        self._heat_time = TEMP_0_1

    @property
    def temperature_precision(self) -> str:

        values = ("HIGH_PRECISION", "MEDIUM_PRECISION", "LOW_PRECISION")
        return values[self._temperature_precision]

    @temperature_precision.setter
    def temperature_precision(self, value: int) -> None:
        if value not in temperature_precision_values:
            raise ValueError("Value must be a valid temperature_precision setting")
        self._temperature_precision = value
        self._command = temperature_precision_values[value]

    @property
    def relative_humidity(self) -> float:

        return self.measurements[1]

    @property
    def temperature(self) -> float:
        """The current temperature in Celsius"""
        return self.measurements[0]

    @property
    def measurements(self) -> Tuple[float, float]:
        """both `temperature` and `relative_humidity`, read simultaneously
        If you use t the heater function, sensor will be not give a response
        back. Waiting time is added to the logic to account for this situation
        """

        self._i2c.writeto(self._address, bytes([self._command]), False)
        if self._command in (0x39, 0x2F, 0x1E):
            time.sleep(1.2)
        elif self._command in (0x32, 0x24, 0x15):
            time.sleep(0.2)
        time.sleep(0.2)
        self._i2c.readfrom_into(self._address, self._data)

        temperature, temp_crc, humidity, humidity_crc = struct.unpack_from(
            ">HBHB", self._data
        )

        if temp_crc != self._crc(
            memoryview(self._data[0:2])
        ) or humidity_crc != self._crc(memoryview(self._data[3:5])):
            raise RuntimeError("Invalid CRC calculated")

        temperature = -45.0 + 175.0 * temperature / 65535.0

        humidity = -6.0 + 125.0 * humidity / 65535.0
        humidity = max(min(humidity, 100), 0)

        return temperature, humidity

    @staticmethod
    def _crc(buffer) -> int:
        """verify the crc8 checksum"""
        crc = 0xFF
        for byte in buffer:
            crc ^= byte
            for _ in range(8):
                if crc & 0x80:
                    crc = (crc << 1) ^ 0x31
                else:
                    crc = crc << 1
        return crc & 0xFF

    @property
    def heater_power(self) -> str:
        values = ("HEATER200mW", "HEATER110mW", "HEATER20mW")
        return values[self._heater_power]

    @heater_power.setter
    def heater_power(self, value: int) -> None:
        if value not in heater_power_values:
            raise ValueError("Value must be a valid heater power setting")
        self._heater_power = value
        self._command = wat_config[value][self._heat_time]

    @property
    def heat_time(self) -> str:
        values = ("TEMP_1", "TEMP_0_1")
        return values[self._heat_time]

    @heat_time.setter
    def heat_time(self, value: int) -> None:
        if value not in heat_time_values:
            raise ValueError("Value must be a valid heat_time setting")
        self._heat_time = value
        self._command = wat_config[self._heater_power][value]

    def reset(self):
        """
        Reset the sensor
        """
        self._i2c.writeto(self._address, bytes([_RESET]), False)
        time.sleep(0.1)
