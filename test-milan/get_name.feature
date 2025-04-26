Feature: Returns name for a specific descriptor

Scenario: Get Entity Name
Given the entity is online and running
When the get_name command with the correctly popluated get_name frame is sent
Then the get_name_response is expected within 200ms
    And the get_name response with thee correctly populated fields is received
    And the received name is matching the entity name

# Sent frame: b'\x91\xe0\xf0\x01\x00\x00\x00\xe0Lh\x01\x83"\xf0\xfb\x00\x00\x18\x00\x1b!\xff\xfe\xde\xdf\xd9\xb1k\x00\xb5\x00\x00\x00\x00\x00\x00\x00\x11\x00\x00\x00\x00\x00\x00\x00\x00'
# TODO: negative testing (different entity id: no response, out of bounds) ...