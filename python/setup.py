# -*- coding: utf-8 -*-

import setuptools

with open("README.md", "r") as fh:
    long_description = fh.read()

setuptools.setup(
    name="rts2",
    version="0.1.0",
    author="Petr Kubánek",
    author_email="petr@rts2.org",
    description="RTS2 Python client scripting support",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/RTS2/rts2",
    packages=setuptools.find_packages(),
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: GNU Lesser General Public License v2 or later (LGPLv2+)",
        "Operating System :: OS Independent",
    ],
    python_requires='>=3.6',
)
