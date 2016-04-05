package org.libsdl.app;

import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Created by Wakim on 16/4/5.
 */
public class SDLHandler {

    private static ConcurrentLinkedQueue<SDLEvent> mQueue = new ConcurrentLinkedQueue<>();

    public static void runOnSDLThread(SDLEvent event) {
        mQueue.add(event);
    }

    public static void handle() {
        while (!mQueue.isEmpty()) {
            mQueue.poll();
        }
    }
}
