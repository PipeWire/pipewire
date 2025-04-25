Feature: Entity lock timeout handling

Scenario: Lock on entity does timeout after 60s
Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the entity is initially unlocked
When Hive Controller no. 1 locks the entity
Then Hive Controller no. 1 displays a green lock symbol for the entity
    And Hive Controller no. 2 displays a red lock symbol for the entity
When 60 seconds have passed since the lock was acquired
Then both Hive Controllers should display an unlocked symbol for the entity, indicating that the lock has timed out after 60 seconds.

Scenario: Lock entity on already locked entity
Given Hive Controller no. 1 is running with sub ID 130
    And Hive Controller no. 2 is running with sub ID 140
    And the entity is initially unlocked
When Hive Controller no. 1 locks the entity
Then Hive Controller no. 1 displays a green lock symbol for the entity
    And Hive Controller no. 2 displays a red lock symbol for the entity
When Hive Controller no. 2 locks the entity
Then Hive Controller no. 2 reports "Lock Entity failed: The AVDECC Entity has been locked by another AVDECC Controller."