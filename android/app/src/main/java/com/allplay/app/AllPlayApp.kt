package com.allplay.app

import android.app.Application
import android.view.Surface

/**
 * Process-level singleton: lets MainActivity hand the Surface to the resident service (avoiding a complex Binder).
 */
class AllPlayApp : Application() {
    var service: AllPlaySessionService? = null
    var surface: Surface? = null
}
