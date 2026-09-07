const GodotJSFileSystemLibrary = {
	$GodotJSFileSystem__deps: ['$GodotFS'],
	// GodotJS packages execute in the browser global realm, while Emscripten's
	// filesystem remains private to the modularized template. Expose only this
	// callback-based operation; keep the FS object itself module-private and
	// serialize with Godot's automatic IndexedDB persistence.
	$GodotJSFileSystem__postset: 'globalThis["GodotJSFileSystem"] = GodotJSFileSystem;',
	$GodotJSFileSystem: {
		"sync": function (callback) {
			if (typeof callback !== 'function') {
				return false;
			}

			let completed = false;
			const finish = function (error) {
				if (completed) {
					return;
				}
				completed = true;
				callback(error || null);
			};

			try {
				if (typeof GodotFS === 'undefined' || typeof GodotFS.sync !== 'function') {
					finish(new Error('Godot Web filesystem synchronization is unavailable'));
					return true;
				}
				GodotFS.sync().then(finish, finish);
			} catch (error) {
				finish(error);
			}
			return true;
		},
	},
};

mergeInto(LibraryManager.library, GodotJSFileSystemLibrary);
