/* Gstreamer
 * Copyright (C)  2022 Pexip (https://pexip.com/)
 *   @author: Tulio Beloqui <tulio@pexip.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

package org.freedesktop.gstreamer.androidmedia;

import android.media.AudioDeviceInfo;
import android.media.AudioDeviceCallback;

public class GstAmAudioDeviceCallback extends AudioDeviceCallback
{
    private long context = 0;

    public void setContext(long c) {
    	context = c;
    }

    public synchronized void onAudioDevicesAdded(AudioDeviceInfo[] addedDevices) {
    	native_onAudioDevicesAdded(context, addedDevices);
    }

    public synchronized void onAudioDevicesRemoved(AudioDeviceInfo[] removedDevices) {
    	native_onAudioDevicesRemoved(context, removedDevices);
    }

    private native void native_onAudioDevicesAdded (long context, AudioDeviceInfo[] addedDevices);

    private native void native_onAudioDevicesRemoved (long context, AudioDeviceInfo[] addedDevices);
}
