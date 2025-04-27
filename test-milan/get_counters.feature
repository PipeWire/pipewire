Feature: Returns counters for a specific descriptor

# Note: not mandatory for Milan v1.2
# Scenario: Get Entity Counters
# Given the entity with descriptor type "ENTITY" and descriptor index "0" is online and running
# When the get_counters command is sent with a correctly populated frame (descriptor_type=ENTITY, descriptor_index=0)
# Then the get_counters_response is received within 200ms
#     And the get_counters_response response with the correctly populated fields is received
# # Sent frame: '91:e0:f0:01:00:00:00:e0:4c:68:01:83:22:f0:fb:00:00:18:00:1b:21:ff:fe:de:df:d9:b1:6b:00:b5:00:00:00:00:00:00:00:29:00:00:00:00'
# # Received frame: '00e04c680183001b21dedfd922f0fb010094001b21fffededfd9b16b00b5000000000000002900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'

Scenario: Get AVB_INTERFACE Counters
Given the entity with descriptor type "AVB_INTERFACE" and descriptor index "0" is online and running
When the get_counters command is sent with a correctly populated frame (descriptor_type=AVB_INTERFACE, descriptor_index=0)
Then the get_counters_response is received within 200ms
    And the get_counters_response response with the correctly populated fields is received
# Milan v1.2, Table 5.13: GET_COUNTERSmandatory AVB Interface counters.
# Expected
# Bit Bit value Symbol
# 31 0x00000001 LINK_UP
# 30 0x00000002 LINK_DOWN
# 26 0x00000020 GPTP_GM_CHANGED
# Sent: '91:e0:f0:01:00:00:00:e0:4c:68:01:83:22:f0:fb:00:00:18:00:1b:21:ff:fe:de:df:d9:b1:6b:00:b5:00:00:00:00:00:00:00:29:00:09:00:00'
# Received: '00e04c680183001b21dedfd922f0fb010094001b21fffededfd9b16b00b5000000000000002900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'

Scenario: Get CLOCK_DOMAIN Counters
Given the entity with descriptor type "CLOCK_DOMAIN" and descriptor index "0" is online and running
When the get_counters command is sent with a correctly populated frame (descriptor_type=CLOCK_DOMAIN, descriptor_index=0)
Then the get_counters_response is received within 200ms
    And the get_counters_response response with the correctly populated fields is received
# Milan v1.2, Table 5.15: GET_COUNTERS Clock Domain counters.
# Expected
# Bit Bit value Symbol
# 31 0x00000001 LOCKED
# 30 0x00000002 UNLOCKED
# Sent: '91:e0:f0:01:00:00:00:e0:4c:68:01:83:22:f0:fb:00:00:18:00:1b:21:ff:fe:de:df:d9:b1:6b:00:b5:00:00:00:00:00:00:00:29:00:24:00:00'
# Received: '00e04c680183001b21dedfd922f0fb010094001b21fffededfd9b16b00b5000000000000002900240000000000030000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'

Scenario: Get STREAM_INPUT Counters
Given the entity with descriptor type "STREAM_INPUT" and descriptor index "0" is online and running
When the get_counters command is sent with a correctly populated frame (descriptor_type=STREAM_INPUT, descriptor_index=0)
Then the get_counters_response is received within 200ms
    And the get_counters_response response with the correctly populated fields is received
# Milan v1.2, Table 5.16: GET_COUNTERS Stream Input counters.
# Expected
# Bit Bit value Symbol
# 31 0x00000001 MEDIA_LOCKED
# 30 0x00000002 MEDIA_UNLOCKED
# 29 0x00000004 STREAM_INTERRUPTED
# 28 0x00000008 SEQ_NUM_MISMATCH
# 27 0x00000010 MEDIA_RESET
# 26 0x00000020 TIMESTAMP_UNCERTAIN
# 23 0x00000100 UNSUPPORTED_FORMAT 
# 22 0x00000200 LATE_TIMESTAMP 
# 21 0x00000400 EARLY_TIMESTAMP 
# 20 0x00000800 FRAMES_RX 
# Sent: '91:e0:f0:01:00:00:00:e0:4c:68:01:83:22:f0:fb:00:00:18:00:1b:21:ff:fe:de:df:d9:b1:6b:00:b5:00:00:00:00:00:00:00:29:00:05:00:00'
# Received: '00e04c680183001b21dedfd922f0fb010094001b21fffededfd9b16b00b500000000000000290005000000000f1f0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'

Scenario: Get STREAM_OUTPUT Counters
Given the entity with descriptor type "STREAM_OUTPUT" and descriptor index "0" is online and running
When the get_counters command is sent with a correctly populated frame (descriptor_type=STREAM_OUTPUT, descriptor_index=0)
Then the get_counters_response is received within 200ms
    And the get_counters_response response with the correctly populated fields is received
# Milan v1.2, Table 5.17: GET_COUNTERS Stream Output counters.
# Expected
# Bit Bit value Symbol
# 31 0x00000001 STREAM_START 
# 30 0x00000002 STREAM_STOP 
# 29 0x00000004 MEDIA_RESET  
# 28 0x00000008 TIMESTAMP_UNCERTAIN 
# 27 0x00000010 FRAMES_TX 

# TODO: negative testing (out of bounds), optional counters for AVB Interface