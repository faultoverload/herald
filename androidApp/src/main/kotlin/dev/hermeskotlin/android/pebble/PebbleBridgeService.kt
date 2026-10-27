package dev.hermeskotlin.android.pebble

import android.util.Log
import dev.hermeskotlin.android.notify.ChatNotifier
import dev.hermeskotlin.core.settings.SettingsStore
import dev.hermeskotlin.core.wear.WatchRelay
import dev.hermeskotlin.ui.voice.watchAccent
import dev.hermeskotlin.wearlink.PebbleIncoming
import dev.hermeskotlin.wearlink.PebbleKeys
import dev.hermeskotlin.wearlink.PebbleLink
import dev.hermeskotlin.wearlink.PebbleLink.toWatchAsk
import io.rebble.pebblekit2.client.BasePebbleListenerService
import io.rebble.pebblekit2.common.model.PebbleDictionary
import io.rebble.pebblekit2.common.model.ReceiveResult
import io.rebble.pebblekit2.common.model.WatchIdentifier
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.koin.android.ext.android.inject
import java.util.UUID

/**
 * Where Herald's Pebble watchapp lands on the phone, even with Herald closed: the Pebble phone app binds this while
 * the watchapp is open, and hands it what the watch sends. Each message goes to [WatchRelay], as a Wear OS watch's
 * does. Only the Pebble phone app Herald picked ([PebbleApps]) gets through: the base class checks the caller.
 */
class PebbleBridgeService : BasePebbleListenerService() {

    private val relay: WatchRelay by inject()
    private val outbox: PebbleOutbox by inject()
    private val notifier: ChatNotifier by inject()
    private val settings: SettingsStore by inject()
    private val scope: CoroutineScope by inject()

    // Called on the main thread.
    override suspend fun onMessageReceived(watchappUUID: UUID, data: PebbleDictionary, watch: WatchIdentifier): ReceiveResult {
        if (watchappUUID != PebbleOutbox.APP_UUID) return ReceiveResult.Nack
        val node = PebbleOutbox.nodeOf(watch)
        // One this phone doesn't understand is still taken, so the watch doesn't send it again and again.
        val dict = data.toPebbleLink()
        val incoming = PebbleLink.decode(dict) ?: return ReceiveResult.Ack
        // By type number: R8 renames the classes in a release build.
        Log.d(TAG, "From the Pebble: type ${dict[PebbleKeys.TYPE]}")
        when (incoming) {
            is PebbleIncoming.Ask -> {
                relay.ask(node, incoming.toWatchAsk())
                // Started now, while the message is being delivered: the turn may outlast the watchapp's binding.
                notifier.startServiceNow()
            }
            is PebbleIncoming.Stop -> {
                relay.stop(incoming.askId)
                outbox.watch(node).stopped(incoming.askId)
            }
            PebbleIncoming.ChatsRequest -> relay.chats(node)
            is PebbleIncoming.StatusRequest -> scope.launch {
                // Waited for: the watch's first message often starts Herald, and the stored settings are still being read then.
                val accent = watchAccent(settings.settings.filterNotNull().first().accent)
                relay.status(node, accent = accent.updateFor(incoming.accentRevision))
            }
            is PebbleIncoming.Resync -> outbox.watch(node).resync(incoming.askId)
            // Rings come with the watchapp's ring screen.
            is PebbleIncoming.RingOn, is PebbleIncoming.RingEnd -> Unit
        }
        return ReceiveResult.Ack
    }

    override fun onAppOpened(watchappUUID: UUID, watch: WatchIdentifier) {
        if (watchappUUID == PebbleOutbox.APP_UUID) outbox.watch(PebbleOutbox.nodeOf(watch)).opened()
    }

    override fun onAppClosed(watchappUUID: UUID, watch: WatchIdentifier) {
        if (watchappUUID == PebbleOutbox.APP_UUID) outbox.watch(PebbleOutbox.nodeOf(watch)).closed()
    }

    private companion object {
        const val TAG = "HeraldPebble"
    }
}
