from subprocess import check_output, CalledProcessError, STDOUT
from pytest import raises
from re import match


def test_leaking_library():
    with raises(CalledProcessError) as exception_info:
        check_output(['dynamic_loader'], stderr=STDOUT)

    assert exception_info.value.returncode == 1

    header = 'overthrower has detected not freed memory blocks with following addresses:'
    footer = '^^^^^^^^^^^^^^^^^^  |  ^^^^^^  |  ^^^^^^^^^^'
    output = exception_info.value.output.decode().rstrip().splitlines()
    assert header in output and footer in output

    header_index = output.index(header)
    footer_index = output.index(footer)
    assert header_index < footer_index

    leaked_blocks = output[header_index + 1:footer_index]
    assert len(leaked_blocks) == 1  # Exactly one memory leak is expected to be detected.

    match_object = match(r'^0x[\da-f]{16}\s+-\s+\d+\s+-\s+(\d+)$', leaked_blocks[0])
    assert match_object is not None
    assert int(match_object.group(1)) == 731465028  # Exactly this amount of bytes is allocated and never freed at `leaking_library`.
