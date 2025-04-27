Feature: Modify the stream format of a STREAM_INPUT or STREAM_OUTPUT descriptor

Scenario: Change the stream format of a Talker
Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the entity is discovered and available in Hive Controller no. 1 and Hive Controller no. 2, running on the default stream format
# TODO: Stream formats are either a dropdown list or defined with the up to bit, which does not allow 
When Hive Controller no. 1 changes the Talker stream format to the second format of the available stream formats
Then Hive Controller no. 1 reports the second stream format in the Current Stream Format row
    And Hive Controller no. 1 reports the second stream format in the Stream Format dropdown menu
    And Hive Controller no. 2 reports the second stream format in the Current Stream Format row
    And Hive Controller no. 2 reports the second stream format in the Stream Format dropdown menu
    And the AEM unsolicited responses counter in Hive Controller no. 2 is increased by 1

Scenario: Change the stream format of a listener
Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the entity is discovered and available in Hive Controller no. 1 and Hive Controller no. 2, running on the default stream format
# TODO: Stream formats are either a dropdown list or defined with the up to bit, which does not allow 
When Hive Controller no. 1 changes the Listener stream format to the second format of the available stream formats
Then Hive Controller no. 1 reports the second stream format in the Current Stream Format row
    And Hive Controller no. 1 reports the second stream format in the Stream Format dropdown menu
    And Hive Controller no. 2 reports the second stream format in the Current Stream Format row
    And Hive Controller no. 2 reports the second stream format in the Stream Format dropdown menu
    And the AEM unsolicited responses counter in Hive Controller no. 2 is increased by 1

# TODO: Add tests for locked entity, out of bound change of stream format, ...