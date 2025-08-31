function decodeUplink(input) {
  var data = {};
  data.Temperature = input.bytes[0];
  data.Humidity = input.bytes[1];
  data.Bat_vol = (input.bytes[2] * 7.059)+2500;
  data.Luminosity =  ((input.bytes[4] << 8) + (input.bytes[3]))
  data.AccX = input.bytes[5] * 39.216;
  data.AccY = input.bytes[6] * 39.216;
  data.AccZ = input.bytes[7] * 39.216;
  data.Lat = (((input).bytes[11] << 24 ) + (input.bytes[10] << 16) + (input.bytes[9] << 8) + (input.bytes[8])) / 1000000
  data.Lon = (((input).bytes[15] << 24 ) + (input.bytes[14] << 16) + (input.bytes[13] << 8) + (input.bytes[12])) / 1000000
  data.Alt = ((input.bytes[17] << 8) + (input.bytes[16]))
  data.Sat = input.bytes[18]
  data.Time = (((input).bytes[22] << 24 ) + (input.bytes[21] << 16) + (input.bytes[20] << 8) + (input.bytes[19]))

  return {
    data: data,
    };
  }