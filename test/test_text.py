import sys
from .. import text

def test_setlocale(test):
	"""
	# - &.extensions.text/module.c#dsetlocale
	# - &text.setlocale
	"""
	import locale
	test/text.setlocale() == locale.setlocale(locale.LC_ALL, "")

def test_cells(test):
	"""
	# - &.extensions.text/module.c#cells
	# - &text.cells
	"""
	cells = text.cells

	# ascii fast path
	test/cells("") == 0
	test/cells("ascii") == len("ascii")

	# combining characters should have no width
	test/cells("a\u0350scii") == len("ascii")
	test/cells("a\u0350 z\u0350") == 3

def test_cells_chinese_sample(test):
	"""
	# - &.extensions.text/module.c#cells
	# - &text.cells
	"""
	cells = text.cells
	test/cells("谢谢你的春天") == len("谢谢你的春天")*2

def test_cells_ctlchars(test):
	"""
	# - &.extensions.text/module.c#cells
	# - &text.cells
	"""
	cells = text.cells

	# fast path skip lets this through
	test/cells("\x00\x01") == 2

	# No fast path skip, so it generates an error.
	test/cells("\x02\x01 春") == -1

def test_encoding(test):
	locale = text.setlocale()
	text.encoding() in test/locale
