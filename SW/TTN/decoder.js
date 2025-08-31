function decodeUplink(input) {
  var buf = new ArrayBuffer(8);
  var view = new DataView(buf);
  var data = {};
  data.Temperature = input.bytes[0];
  data.Humidity = input.bytes[1];
  data.Bat_vol = input.bytes[2] * 17.647;
  data.Luminosity = input.bytes[3] * 19.607;
  data.AccX = input.bytes[4] * 39.216;
  data.AccY = input.bytes[5] * 39.216;
  data.AccZ = input.bytes[6] * 39.216;
  data.Lat = (((input).bytes[10] << 24 ) + (input.bytes[9] << 16) + (input.bytes[8] << 8) + (input.bytes[7])) / 1000000
  data.Lon = (((input).bytes[14] << 24 ) + (input.bytes[13] << 16) + (input.bytes[12] << 8) + (input.bytes[11])) / 1000000
  data.Alt = ((input.bytes[15] << 8) + (input.bytes[16]))
  data.Sat = input.bytes[17]
  data.Time = (((input).bytes[21] << 24 ) + (input.bytes[20] << 16) + (input.bytes[19] << 8) + (input.bytes[18]))
  return {
    data: data,
    };
  }
