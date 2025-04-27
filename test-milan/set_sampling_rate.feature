Feature: Sample rate change of the audio unit

Scenario: Audio unit changes sample rate to non-default sample rate

Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the entity is discovered and available in Hive Controller no. 1 and Hive Controller no. 2, running on the default sample rate
When The sample rate for the audio unit is changed to the non-default sample rate in Hive Controller no. 1
Then Hive Controller no. 1 shows the new sample rate
    And Hive Controller no. 2 reports the sample rate change
    And the AEM unsolicited responses counter in Hive Controller no. 2 is increased by 1