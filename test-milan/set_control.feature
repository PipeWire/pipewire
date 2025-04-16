Feature: Change values of a control

Scenario: Identify device using the Identify descriptor
GIVEN Hive Controller no. 1 and Hive Controller no. 2 are running with different controller sub IDs (e.g., 130 and 140)
    AND the AVDECC entity is online and in an idle state
WHEN The entity is selected in Hive Controller no. 1 and "Identify Device (10s)" is selected from the context menu
THEN The entity ID is displayed in bold font in the UI of Hive Controller no. 1 for 10 seconds
    AND The entity ID is displayed in bold font in the UI of Hive Controller no. 2 for 10 seconds
    AND after 10 seconds, the entity ID is not displayed in bold font in the UI of Hive Controller no. 1
    AND the entity ID is not displayed in bold font in the UI of Hive Controller no. 2
