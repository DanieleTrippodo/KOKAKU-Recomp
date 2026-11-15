# A rebuild must preserve the player's settings.
if(NOT EXISTS "${DESTINATION}")
    configure_file("${SOURCE}" "${DESTINATION}" COPYONLY)
endif()
