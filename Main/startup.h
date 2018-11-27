#ifndef __STARTUP_H__
#define __STARTUP_H__

#ifdef __cplusplus
 extern "C" {
#endif

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)  \
  (byte & 0x20 ? '1' : '0'), \
  (byte & 0x10 ? '1' : '0'), \
  (byte & 0x08 ? '1' : '0'), \
  (byte & 0x04 ? '1' : '0'), \
  (byte & 0x02 ? '1' : '0'), \
  (byte & 0x01 ? '1' : '0')

 void initialize();
 void setup();
 void loop();

#ifdef __cplusplus
}
#endif

#endif /* __STARTUP_H__ */
