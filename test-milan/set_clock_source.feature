Feature: Clock source configuration of an entity

Scenario: Change the clock source of the current configuration
Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the entity is discovered and available in Hive Controller no. 1 and Hive Controller no. 2, running on the default clock source
When Hive Controller no. 1 changes the clock source of the current configuration to the second clock source of the available clock sources.
Then Hive Controller no. 1 reports the second clock source
    And Hive Controller no. 2 reports the second clock source
    And the AEM unsolicited responses counter in Hive Controller no. 2 is increased by 1

# TODO: Add tests for locked entity and negative tests: out of bounds 