PY3_LIBRARY()



NO_WSHADOW()

PEERDIR(
    contrib/tools/python3/src
    contrib/tools/python3/lib/py
    library/resource
)

CFLAGS(-DCYTHON_REGISTER_ABCS=0)

NO_PYTHON_INCLUDES()

ENABLE(PYBUILD_NO_PYC)

PY_SRCS(
    entry_points.py
    TOP_LEVEL
    __res.pyx
)


RESOURCE_FILES(
    PREFIX ${MODDIR}/
    __res.pyx
    importer.pxi
    sitecustomize.pyx
)

NO_LINT()

END()

RECURSE_FOR_TESTS(
    test
)
