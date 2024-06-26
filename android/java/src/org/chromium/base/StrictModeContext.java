// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.base;

import android.os.Build;
import android.os.StrictMode;

import java.io.Closeable;

/**
 * Enables try-with-resources compatible StrictMode violation allowlisting.
 *
 * Prefer "ignored" as the variable name to appease Android Studio's "Unused symbol" inspection.
 *
 * Example:
 * <pre>
 *     try (StrictModeContext ignored = StrictModeContext.allowDiskWrites()) {
 *         return Example.doThingThatRequiresDiskWrites();
 *     }
 * </pre>
 *
 */
public final class StrictModeContext implements Closeable {
    private final StrictMode.ThreadPolicy mThreadPolicy;
    private final StrictMode.VmPolicy mVmPolicy;

    private StrictModeContext(StrictMode.ThreadPolicy threadPolicy, StrictMode.VmPolicy vmPolicy) {
        // TODO(crbug.com/40279700): Determine after auditing strict mode context usage if we should
        // keep
        // or remove these trace events.
        TraceEvent.startAsync("StrictModeContext", hashCode());
        mThreadPolicy = threadPolicy;
        mVmPolicy = vmPolicy;
    }

    private StrictModeContext(StrictMode.ThreadPolicy threadPolicy) {
        this(threadPolicy, null);
    }

    private StrictModeContext(StrictMode.VmPolicy vmPolicy) {
        this(null, vmPolicy);
    }

    /**
     * Convenience method for disabling all VM-level StrictMode checks with try-with-resources.
     * Includes everything listed here:
     *     https://developer.android.com/reference/android/os/StrictMode.VmPolicy.Builder.html
     */
    public static StrictModeContext allowAllVmPolicies() {
        try (TraceEvent e = TraceEvent.scoped("StrictModeContext.allowAllVmPolicies")) {
            StrictMode.VmPolicy oldPolicy = StrictMode.getVmPolicy();
            StrictMode.setVmPolicy(StrictMode.VmPolicy.LAX);
            return new StrictModeContext(oldPolicy);
        }
    }

    /**
     * Convenience method for disabling all thread-level StrictMode checks with try-with-resources.
     * Includes everything listed here:
     *     https://developer.android.com/reference/android/os/StrictMode.ThreadPolicy.Builder.html
     */
    public static StrictModeContext allowAllThreadPolicies() {
        try (TraceEvent e = TraceEvent.scoped("StrictModeContext.allowAllThreadPolicies")) {
            StrictMode.ThreadPolicy oldPolicy = StrictMode.getThreadPolicy();
            StrictMode.setThreadPolicy(StrictMode.ThreadPolicy.LAX);
            return new StrictModeContext(oldPolicy);
        }
    }

    /** Convenience method for disabling StrictMode for disk-writes with try-with-resources. */
    public static StrictModeContext allowDiskWrites() {
        try (TraceEvent e = TraceEvent.scoped("StrictModeContext.allowDiskWrites")) {
            StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskWrites();
            return new StrictModeContext(oldPolicy);
        }
    }

    /** Convenience method for disabling StrictMode for disk-reads with try-with-resources. */
    public static StrictModeContext allowDiskReads() {
        try (TraceEvent e = TraceEvent.scoped("StrictModeContext.allowDiskReads")) {
            StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskReads();
            return new StrictModeContext(oldPolicy);
        }
    }

    /** Convenience method for disabling StrictMode for slow calls with try-with-resources. */
    public static StrictModeContext allowSlowCalls() {
        try (TraceEvent e = TraceEvent.scoped("StrictModeContext.allowSlowCalls")) {
            StrictMode.ThreadPolicy oldPolicy = StrictMode.getThreadPolicy();
            StrictMode.setThreadPolicy(
                    new StrictMode.ThreadPolicy.Builder(oldPolicy).permitCustomSlowCalls().build());
            return new StrictModeContext(oldPolicy);
        }
    }

    /**
     * Convenience method for disabling StrictMode for unbuffered input/output operations with
     * try-with-resources.
     * For API level 25- this method will do nothing;
     * because StrictMode.ThreadPolicy.Builder#permitUnbufferedIo is added in API level 26.
     */
    public static StrictModeContext allowUnbufferedIo() {
        try (TraceEvent e = TraceEvent.scoped("StrictModeContext.allowUnbufferedIo")) {
            StrictMode.ThreadPolicy oldPolicy = StrictMode.getThreadPolicy();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                StrictMode.setThreadPolicy(
                        new StrictMode.ThreadPolicy.Builder(oldPolicy)
                                .permitUnbufferedIo()
                                .build());
            }
            return new StrictModeContext(oldPolicy);
        }
    }

    @Override
    public void close() {
        if (mThreadPolicy != null) {
            StrictMode.setThreadPolicy(mThreadPolicy);
        }
        if (mVmPolicy != null) {
            StrictMode.setVmPolicy(mVmPolicy);
        }
        TraceEvent.finishAsync("StrictModeContext", hashCode());
    }
}
