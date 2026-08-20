# hw-lilygo-tbeam-supreme board HAL — an ordinary spangap component.
#
# Compiles the LilyGo T-Beam S3 Supreme board code: the AXP2101 PMU rail
# bring-up and the peripheral-bus SD/IMU CS park (tbeamsupreme.cpp).
#
# app_main is NOT here — it's generated into the reticulous/reticulous buildable
# that adds this board with `--with`. This component just exports the board
# Service (TbeamSupremeBoard) the generated dispatcher constructs and walks.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS
        "src/tbeamsupreme.cpp"
        # This board's one self-assertion (detect_hw), called by
        # spangapInit() before any bus is claimed.
        "src/detect.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
)
