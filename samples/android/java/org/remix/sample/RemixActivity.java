package org.remix.sample;

import org.libsdl.app.SDLActivity;

public final class RemixActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
    @Override protected String[] getArguments() {
        return new String[] {
            getFilesDir().getAbsolutePath(),
            getApplicationInfo().nativeLibraryDir + "/libremix.so",
            Integer.toString(getIntent().getIntExtra("frames", 0))
        };
    }
}
