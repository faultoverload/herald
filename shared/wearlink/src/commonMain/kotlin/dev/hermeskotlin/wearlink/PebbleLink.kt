package dev.hermeskotlin.wearlink

/*
 * What Herald on the phone and its Pebble watchapp (pebbleApp/) say to each other. A Pebble talks in AppMessage
 * dictionaries, small maps of numbered keys to integers or text, through the Pebble phone app (PebbleKit Android 2).
 * The phone keeps speaking WearLink's JSON to WatchRelay; the Pebble side of it decodes that JSON per path and
 * writes these dictionaries instead, so the watch never parses JSON. Each dictionary says what it is in [PebbleKeys.TYPE].
 */

/** One value in a Pebble dictionary: every number arrives on the phone as a 32-bit integer, whatever size the watch wrote. */
sealed interface PebbleValue {
    data class Num(val value: Int) : PebbleValue
    data class Text(val value: String) : PebbleValue
}

typealias PebbleDict = Map<Int, PebbleValue>

/** The dictionary keys. pebbleApp/package.json's `messageKeys` holds the same numbers; a test keeps them in step. */
object PebbleKeys {
    const val TYPE = 1
    const val ASK_ID = 2
    const val SEQ = 3
    const val PHASE = 4
    const val TEXT = 5
    /** Where [TEXT] goes in the watch's copy of the reply, in UTF-8 bytes: 0 replaces it, its length appends. */
    const val TEXT_OFFSET = 6
    /** How long the reply is on the phone so far, in UTF-8 bytes: more is coming while the watch has less. */
    const val TEXT_TOTAL = 7
    const val ACTIVITY = 8
    const val TITLE = 9
    const val ERROR = 10
    const val CHAT_ID = 11
    const val INDEX = 12
    const val COUNT = 13
    const val AGO = 14
    const val FLAGS = 15
    const val ACCENT_REV = 16
    const val ACCENT_FILL = 17
    const val ACCENT_TEXT = 18
    const val ACCENT_ON_FILL = 19
    const val ACCENT_DIM = 20
    const val REQUEST_ID = 21
    const val APPROVAL = 22
    const val AGENT = 23
    const val ANSWERED = 24
    const val VERSION = 25

    /** Every key by its name in package.json. */
    val byName: Map<String, Int> = mapOf(
        "TYPE" to TYPE, "ASK_ID" to ASK_ID, "SEQ" to SEQ, "PHASE" to PHASE, "TEXT" to TEXT,
        "TEXT_OFFSET" to TEXT_OFFSET, "TEXT_TOTAL" to TEXT_TOTAL, "ACTIVITY" to ACTIVITY, "TITLE" to TITLE,
        "ERROR" to ERROR, "CHAT_ID" to CHAT_ID, "INDEX" to INDEX, "COUNT" to COUNT, "AGO" to AGO, "FLAGS" to FLAGS,
        "ACCENT_REV" to ACCENT_REV, "ACCENT_FILL" to ACCENT_FILL, "ACCENT_TEXT" to ACCENT_TEXT,
        "ACCENT_ON_FILL" to ACCENT_ON_FILL, "ACCENT_DIM" to ACCENT_DIM, "REQUEST_ID" to REQUEST_ID,
        "APPROVAL" to APPROVAL, "AGENT" to AGENT, "ANSWERED" to ANSWERED, "VERSION" to VERSION,
    )
}

/** What a dictionary is ([PebbleKeys.TYPE]). Below 64 the watch sends it, from 64 the phone does. */
object PebbleTypes {
    /** [PebbleKeys.ASK_ID], [PebbleKeys.TEXT] (dictated or picked), and [PebbleKeys.CHAT_ID] unless it's a new chat. */
    const val ASK = 1
    const val STOP = 2
    const val CHATS_REQ = 3
    /** With [PebbleKeys.ACCENT_REV], the accent the watch has; the phone answers with [STATUS]. */
    const val STATUS_REQ = 4
    const val RING_ON = 5
    const val RING_END_FROM_WATCH = 6
    /** The watch was told to add text past what it has (it restarted unseen): the phone sends the reply again from 0. */
    const val RESYNC = 7

    const val REPLY = 64
    /** One of the recent chats, [PebbleKeys.INDEX] of [PebbleKeys.COUNT]; a count of 0 is an empty list or its [PebbleKeys.ERROR]. */
    const val CHAT_ITEM = 65
    const val STATUS = 66
    const val RING = 67
    const val RING_END = 68
}

/** What the watch sent, read from its dictionary. */
sealed interface PebbleIncoming {
    data class Ask(val id: String, val text: String, val chatId: String?) : PebbleIncoming
    data class Stop(val askId: String) : PebbleIncoming
    data object ChatsRequest : PebbleIncoming
    data class StatusRequest(val accentRevision: String?) : PebbleIncoming
    data class RingOn(val requestId: String) : PebbleIncoming
    data class RingEnd(val requestId: String, val answered: Boolean) : PebbleIncoming
    data class Resync(val askId: String) : PebbleIncoming
}

object PebbleLink {
    /** The watchapp's UUID, as in pebbleApp/package.json. */
    const val APP_UUID = "5f539f5a-9395-4cf1-8601-c3882e672ccd"

    /** Goes up when a message changes in a way an older watchapp would misread. */
    const val VERSION = 1

    /** The most of a reply the watch keeps, in UTF-8 bytes; it has 128 KB for its code and all its memory. */
    const val MAX_REPLY_BYTES = 6_000

    /**
     * The most reply text one message carries, in UTF-8 bytes. With a reply's other fields at their longest the
     * message stays well inside the 2 KB inbox the watchapp asks for.
     */
    const val CHUNK_BYTES = 1_024

    /** The inbox the watchapp opens, in bytes; [size] of every message the phone sends must fit. */
    const val WATCH_INBOX_BYTES = 2_048

    private const val MAX_TITLE_BYTES = 60
    private const val MAX_ACTIVITY_BYTES = 60
    private const val MAX_ERROR_BYTES = 120

    /** [dict]'s size on the watch, as `dict_calc_buffer_size` counts it: a 7-byte header per value, text with its NUL. */
    fun size(dict: PebbleDict): Int = 1 + dict.values.sumOf {
        7 + when (it) {
            is PebbleValue.Num -> 4
            is PebbleValue.Text -> utf8Size(it.value) + 1
        }
    }

    /** What the watch sent, or null for a dictionary this phone doesn't understand (a newer watchapp, a stray key). */
    fun decode(dict: PebbleDict): PebbleIncoming? {
        fun text(key: Int) = (dict[key] as? PebbleValue.Text)?.value
        fun num(key: Int) = (dict[key] as? PebbleValue.Num)?.value
        return when (num(PebbleKeys.TYPE)) {
            PebbleTypes.ASK -> PebbleIncoming.Ask(
                id = text(PebbleKeys.ASK_ID)?.takeIf { it.isNotEmpty() } ?: return null,
                text = text(PebbleKeys.TEXT).orEmpty(),
                chatId = text(PebbleKeys.CHAT_ID)?.takeIf { it.isNotEmpty() },
            )
            PebbleTypes.STOP -> PebbleIncoming.Stop(text(PebbleKeys.ASK_ID) ?: return null)
            PebbleTypes.CHATS_REQ -> PebbleIncoming.ChatsRequest
            PebbleTypes.STATUS_REQ -> PebbleIncoming.StatusRequest(text(PebbleKeys.ACCENT_REV)?.takeIf { it.isNotEmpty() })
            PebbleTypes.RING_ON -> PebbleIncoming.RingOn(text(PebbleKeys.REQUEST_ID) ?: return null)
            PebbleTypes.RING_END_FROM_WATCH -> PebbleIncoming.RingEnd(text(PebbleKeys.REQUEST_ID) ?: return null, num(PebbleKeys.ANSWERED) == 1)
            PebbleTypes.RESYNC -> PebbleIncoming.Resync(text(PebbleKeys.ASK_ID) ?: return null)
            else -> null
        }
    }

    /** The watch's ask, for the relay: never read aloud, since a Pebble has no voice to play it in. */
    fun PebbleIncoming.Ask.toWatchAsk(): WatchAsk = WatchAsk(id = id, text = text, chatId = chatId, speak = false)

    /**
     * One message of [update]'s turn: its state, and [chunk] of its plain text placed at byte [offset] of the
     * [total] bytes the phone has so far.
     */
    fun reply(update: ReplyUpdate, offset: Int, chunk: String, total: Int): PebbleDict = buildMap {
        put(PebbleKeys.TYPE, num(PebbleTypes.REPLY))
        put(PebbleKeys.ASK_ID, text(update.askId))
        put(PebbleKeys.SEQ, num(update.seq))
        put(PebbleKeys.PHASE, num(update.phase.ordinal))
        put(PebbleKeys.TEXT_OFFSET, num(offset))
        put(PebbleKeys.TEXT_TOTAL, num(total))
        put(PebbleKeys.TEXT, text(chunk))
        update.chatId?.let { put(PebbleKeys.CHAT_ID, text(it)) }
        update.title?.takeIf { it.isNotBlank() }?.let { put(PebbleKeys.TITLE, text(clipUtf8(it, MAX_TITLE_BYTES))) }
        update.activity?.takeIf { it.isNotBlank() }?.let { put(PebbleKeys.ACTIVITY, text(clipUtf8(it, MAX_ACTIVITY_BYTES))) }
        update.error?.takeIf { it.isNotBlank() }?.let { put(PebbleKeys.ERROR, text(clipUtf8(it, MAX_ERROR_BYTES))) }
    }

    /** How the phone stands, and its accent when the watch's is out of date. */
    fun status(status: PhoneStatus): PebbleDict = buildMap {
        put(PebbleKeys.TYPE, num(PebbleTypes.STATUS))
        put(PebbleKeys.VERSION, num(VERSION))
        var flags = 0
        if (status.signedIn) flags = flags or FLAG_SIGNED_IN
        if (status.connected) flags = flags or FLAG_CONNECTED
        if (status.failed) flags = flags or FLAG_FAILED
        put(PebbleKeys.FLAGS, num(flags))
        status.accent?.let { accent ->
            put(PebbleKeys.ACCENT_REV, text(accent.revision))
            // The watch takes plain RGB (GColorFromHEX); alpha means nothing there.
            put(PebbleKeys.ACCENT_FILL, num(accent.fill and 0xFFFFFF))
            put(PebbleKeys.ACCENT_TEXT, num(accent.text and 0xFFFFFF))
            put(PebbleKeys.ACCENT_ON_FILL, num(accent.onFill and 0xFFFFFF))
            put(PebbleKeys.ACCENT_DIM, num(accent.dim and 0xFFFFFF))
        }
    }

    /** The recent chats, one message each; an empty list (or one that couldn't be read) is one message with a count of 0. */
    fun chats(list: ChatList, nowSeconds: Double): List<PebbleDict> {
        if (list.chats.isEmpty()) {
            return listOf(
                buildMap {
                    put(PebbleKeys.TYPE, num(PebbleTypes.CHAT_ITEM))
                    put(PebbleKeys.COUNT, num(0))
                    list.error?.let { put(PebbleKeys.ERROR, text(clipUtf8(it, MAX_ERROR_BYTES))) }
                },
            )
        }
        return list.chats.mapIndexed { index, chat ->
            buildMap {
                put(PebbleKeys.TYPE, num(PebbleTypes.CHAT_ITEM))
                put(PebbleKeys.INDEX, num(index))
                put(PebbleKeys.COUNT, num(list.chats.size))
                put(PebbleKeys.CHAT_ID, text(chat.id))
                put(PebbleKeys.TITLE, text(clipUtf8(chat.title, MAX_TITLE_BYTES)))
                ago(chat.activityAt, nowSeconds)?.let { put(PebbleKeys.AGO, text(it)) }
            }
        }
    }

    /** Hermes calling for an answer in chat [WatchRing.chatId]. */
    fun ring(ring: WatchRing): PebbleDict = mapOf(
        PebbleKeys.TYPE to num(PebbleTypes.RING),
        PebbleKeys.REQUEST_ID to text(ring.requestId),
        PebbleKeys.CHAT_ID to text(ring.chatId),
        PebbleKeys.TITLE to text(clipUtf8(ring.title, MAX_TITLE_BYTES)),
        PebbleKeys.APPROVAL to num(if (ring.approval) 1 else 0),
        PebbleKeys.AGENT to num(if (ring.agent) 1 else 0),
    )

    fun ringEnd(end: RingEnd): PebbleDict = mapOf(
        PebbleKeys.TYPE to num(PebbleTypes.RING_END),
        PebbleKeys.REQUEST_ID to text(end.requestId),
    )

    /** How long ago [epochSeconds] was, the way a list says it: "now", "5m", "3h", "2d". */
    fun ago(epochSeconds: Double?, nowSeconds: Double): String? {
        val seconds = (nowSeconds - (epochSeconds ?: return null)).toLong()
        return when {
            seconds < 60 -> "now"
            seconds < 3_600 -> "${seconds / 60}m"
            seconds < 86_400 -> "${seconds / 3_600}h"
            else -> "${seconds / 86_400}d"
        }
    }

    const val FLAG_SIGNED_IN = 1
    const val FLAG_CONNECTED = 2
    const val FLAG_FAILED = 4

    private fun num(value: Int) = PebbleValue.Num(value)
    private fun text(value: String) = PebbleValue.Text(value)
}
