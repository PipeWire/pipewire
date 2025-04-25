Feature: Change values of a control

Scenario: Identify device using the Identify descriptor
Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the ATDECC entity is online
    And the ATDECC entity is in an idle state
When The entity is selected in Hive Controller no. 1
    And "Identify Device (10s)" is selected from the context menu
Then The entity ID is displayed in bold font in the UI of Hive Controller no. 1 for 10 seconds
    And The entity ID is displayed in bold font in the UI of Hive Controller no. 2 for 10 seconds
    And after 10 seconds, the entity ID is not displayed in bold font in the UI of Hive Controller no. 1
    And the entity ID is not displayed in bold font in the UI of Hive Controller no. 2

Scenario: Identify device is not possible with locked device
Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the AVDECC entity is online
    And the ATDECC entity is in an idle state
When The entity is selected in Hive Controller no. 1
    And "Lock" is selected from the context menu
Then Hive Controller no. 1 displays a green lock symbol for the entity
    And Hive Controller no. 2 displays a red lock symbol for the entity
When The entity is selected in Hive Controller no. 2
    And "Identify Device (10s)" is selected from the context menu
Then The Hive Controller no. 2 reports "Identify Entity failed: The AVDECC Entity has been locked by another AVDECC Controller."