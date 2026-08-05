@echo off
set MSYSTEM=
set IDF_PYTHON_ENV_PATH=C:\Users\cat\.espressif\python_env\idf5.5_py3.11_env
set "PATH=C:\Users\cat\.espressif\python_env\idf5.5_py3.11_env\Scripts;%PATH%"
call C:\Users\cat\esp\v5.5.2\esp-idf\export.bat
cd /d D:\OBC\OBC
idf.py build
