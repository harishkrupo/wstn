if get_option('remoting')
	user_hint = 'If you rather not build this, set "remoting=false".'

	if not get_option('backend-drm')
		error('Attempting to build the remoting plugin without the required DRM backend. ' + user_hint)
	endif

	depnames = [
		'gstreamer-1.0', 'gstreamer-allocators-1.0',
		'gstreamer-app-1.0', 'gstreamer-video-1.0'
	]
	deps_remoting = [ dep_libweston ]
	foreach depname : depnames
		dep = dependency(depname, required: false)
		if not dep.found()
			error('Remoting plugin requires @0@ which was not found. '.format(depname) + user_hint)
		endif
		deps_remoting += dep
	endforeach

	plugin_remoting = shared_library(
		'remoting-plugin',
		'remoting-plugin.c',
		include_directories: include_directories('..', '../shared'),
		dependencies: deps_remoting,
		name_prefix: '',
		install: true,
		install_dir: dir_module_libweston
	)
	env_modmap += 'remoting-plugin.so=@0@;'.format(plugin_remoting.full_path())
endif
