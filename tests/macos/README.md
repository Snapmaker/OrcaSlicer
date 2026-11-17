# Uninstall watcher lifecycle checks

These standalone targets have no slicer or GUI dependencies. All filesystem operations use a fresh temporary HOME.
It never invokes launchctl or touches real user data.

```sh
cmake -S tests/macos -B build/watcher-lifecycle-tests
cmake --build build/watcher-lifecycle-tests
ctest --test-dir build/watcher-lifecycle-tests --output-on-failure
```

On Windows with a MinGW toolchain such as Strawberry Perl's, keep its `bin` directory (for example
`C:\Strawberry\c\bin`) ahead of other entries in `PATH` when running `ctest`: the binaries import
MinGW runtime DLLs (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`), and an older copy found first on `PATH`
fails to load with status `0xc0000139` (entry point not found).

On all platforms it checks that deleting the application's personal-data directory preserves the coordination file,
and that the coordination directory cannot be passed to the personal-data cleanup API.

The policy target also runs the production command-order and confirmed-cleanup retry code with injected failures.
It checks cancellation before commands, shutdown during initial registration and reload, enable failure,
recovery after bootout errors, failure acknowledgement for bootstrap errors, unchanged agents,
and cleanup retries until installation state changes.

The filesystem target also checks that `.reload.pending` survives a failed attempt and forces a refresh on the
next attempt even when installed files already match. Only acknowledged registration clears the marker.
It exercises the actual UUID serializer/reader, cancellation of a same-path stale confirmation after watch.json
changes, rejection of invalid identities and markers, and rejection of retries superseded by another registration.

On macOS it additionally checks creation of missing lock directories, exclusive ownership through data cleanup,
acquisition after release, and the distinction between contention timeouts and permanent path failures.
Other platforms print a message that native locking checks were skipped.

Native application validation is still required:

1. Hold the lifecycle lock for more than one second, then start the installed application. Verify the GUI opens
   while registration waits. Release the lock and verify the LaunchAgent is registered without restarting the app.
2. Quit the application while registration is waiting. Verify the worker is cancelled and application shutdown completes.
3. Remove the app, answer the uninstall dialog, immediately reinstall and launch it, then remove it again.
   Verify the new registration survives and the second removal prompts again. Exercise both personal-data choices.
4. Verify the coordination directory remains at
   `~/Library/Application Support/com.snapmaker.orca-monitor/` after personal-data cleanup.
5. Switch the application language while registration is waiting for the lifecycle lock. Release the lock and verify
   registration completes without restarting the application.
6. Hold the lifecycle lock while confirming an uninstall. Verify the dialog closes once, cleanup keeps waiting,
   and releasing the lock completes the original choice without another dialog. Also restore the app while waiting
   and verify cleanup is abandoned.
7. Create `Helpers/.reload.pending` with the helper and plist already current, then start the application.
   Verify the existing agent is refreshed and the marker is removed only after successful registration.
8. While cleanup is waiting, reinstall and start the app at the same path, then remove it again. Verify the prior
   confirmation is abandoned because the registration UUID changed; it must not delete the new registration's data.

Cancellation is honored while registration is pending. Once the worker starts updating files, shutdown joins it
through the remaining registration commands instead of interrupting a refresh between bootout and bootstrap.
Each command has its own three-second timeout; filesystem I/O still depends on the operating system.

Each new registration publishes a UUID in watch.json. Retries retain their published UUID, and stop if another
registration or completed cleanup supersedes it. Failed command updates retain the reload marker and retry in
the background. The marker lives in Helpers and is removed with helper files during a completed uninstall;
the persistent lifecycle lock remains outside that cleanup tree.
