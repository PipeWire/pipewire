Feature: Entity lock timeout handling

Scenario: Lock on entity does timeout after 60s
GIVEN Hive Controller no. 1 and Hive Controller no. 2 are running with different controller sub IDs (e.g., 130 and 140)
    AND the entity is initially unlocked
WHEN Hive Controller no. 1 locks the entity
THEN Hive Controller no. 1 displays a green lock symbol for the entity
    AND Hive Controller no. 2 displays a red lock symbol for the entity
WHEN 60 seconds have passed since the lock was acquired
THEN both Hive Controllers should display an unlocked symbol for the entity, indicating that the lock has timed out after 60 seconds.

Scenario: Lock entity on already locked entity
GIVEN Hive Controller no. 1 and Hive Controller no. 2 are running with different controller sub IDs (e.g., 130 and 140)
    AND the entity is initially unlocked
WHEN Hive Controller no. 1 locks the entity
THEN Hive Controller no. 1 displays a green lock symbol for the entity
    AND Hive Controller no. 2 displays a red lock symbol for the entity
WHEN Hive Controller no. 2 locks the entity
THEN Hive Controller no. 2 reports "Lock Entity failed: The AVDECC Entity has been locked by another AVDECC Controller."