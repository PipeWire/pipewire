Feature: Returns name for a specific descriptor

Scenario: Get Entity Name
Given the entity with descriptor type "ENTITY" and descriptor index "0" is online and running
When the get_name command is sent with a correctly populated frame (descriptor_type=ENTITY, descriptor_index=0, name_index=0)
Then the get_name_response is received within 200ms
    And the get_name response with thee correctly populated fields is received
    And the received name is matching the entity name
# Sent frame: b'\x91\xe0\xf0\x01\x00\x00\x00\xe0Lh\x01\x83"\xf0\xfb\x00\x00\x18\x00\x1b!\xff\xfe\xde\xdf\xd9\xb1k\x00\xb5\x00\x00\x00\x00\x00\x00\x00\x11\x00\x00\x00\x00\x00\x00\x00\x00'

Scenario: Get Entity Name from different Entity
Given the entity with descriptor type "ENTITY" and descriptor index "0" is online and running
When the get_name command is sent with a correctly populated frame (descriptor_type=ENTITY2, descriptor_index=0, name_index=0)
Then no response is expected

# TODO: negative testing (out of bounds) ...