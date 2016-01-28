if 1:
	__type__ = 'extension'
	from ...context import import_extension_module
	import_extension_module()
	del import_extension_module
else:
	from ...development import libfactor
	libfactor.load('extension')
