# Pure C codecs of wiki Mower-Gateway-Architecture §3 (layer L2): no ESP-IDF
# or FreeRTOS includes, so test/ builds them on the PC (pio test -e native).
idf_component_register(SRCS "fw_chunk.c" "cobs.c" "crc16.c" "bl_frame.c" "bl_reliable.c" "bl_queue.c"
                       INCLUDE_DIRS "include")
