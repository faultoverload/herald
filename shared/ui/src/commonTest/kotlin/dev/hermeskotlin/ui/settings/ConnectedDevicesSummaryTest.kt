package dev.hermeskotlin.ui.settings

import dev.hermeskotlin.core.settings.AppSettings
import kotlin.test.Test
import kotlin.test.assertEquals

class ConnectedDevicesSummaryTest {

    @Test
    fun theRowSaysWhetherThePebbleIsOnAndAnswers() {
        assertEquals("Pebble on.", connectedDevicesSummary(AppSettings()))
        assertEquals("Pebble on, answering approvals and questions.", connectedDevicesSummary(AppSettings(pebbleAnswers = true)))
        // Off, answering there doesn't count either.
        assertEquals("Pebble off.", connectedDevicesSummary(AppSettings(pebble = false, pebbleAnswers = true)))
    }
}
