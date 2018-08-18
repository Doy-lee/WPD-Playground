@echo off

REM Build for Visual Studio compiler. Run your copy of vcvarsall.bat to setup command-line compiler.
set ProjectName=WPDPlayground
set ProjectNameDLL=%ProjectName%DLL
set BinDir=..\Bin

REM // Check Prerequisites
REM // =============================================================================================
REM Check if build tool is on path
REM >nul, 2>nul will remove the output text from the where command
where /q cl.exe
if %errorlevel%==1 (
	echo MSVC CL not on path, please add it to path to build by command line.
	goto end
)

REM Build tags file if you have ctags in path
where /q ctags
if %errorlevel%==0 (
REM When parsing a C++ member function definition (e.g. "className::function"),
REM ctags cannot determine whether the scope specifier is a class name or
REM a namespace specifier and always lists it as a class name in the scope
REM portion of the extension fields. Also, if a C++ function is defined outside
REM of the class declaration (the usual case), the access specification (i.e.
REM public, protected, or private) and implementation information (e.g. virtual,
REM pure virtual) contained in the function declaration are not known when the
REM tag is generated for the function definition. -c++-kinds=+p fixes that

REM The --fields=+iaS option:
REM    a   Access (or export) of class members
REM    i   Inheritance information
REM    S   Signature of routine (e.g. prototype or parameter list)
REM
REM The --extra=+q option:
REM    By default, ctags only generates tags for separate identifiers found in
REM    the source files. With --extras=+q option, then ctags will also generate
REM    a second, class-qualified tag for each class member
	ctags -R --c++-kinds=+p --fields=+iaS --extras=+q
)

where /q ctime
set HaveCTimeBinary=0
if %errorlevel%==0 (
	set HaveCTimeBinary=1
)

if %HaveCTimeBinary%==1 ctime -begin %ProjectName%.ctm
if not exist %BinDir% mkdir %BinDir%

REM // Compile Switches
REM // =============================================================================================
REM EHa-   disable exception handling (we don't use)
REM GR-    disable c runtime type information RTTI (we don't use)
REM MD[d]  use dynamic runtime library. Add "d" for debug
REM Gm     Minimal rebuild
REM MT[d]  use static runtime library, so build and link it into exe. Add "d" for debug
REM Od     disables optimisations
REM Oi     enable intrinsics optimisation, let us use CPU intrinsics if there is one
REM        instead of generating a call to external library (i.e. CRT).
REM Z[i|7] enables debug data, Z7 combines the debug files into one.
REM W4     warning level 4
REM WX     treat warnings as errors
REM wd4100 unused argument parameters
REM wd4201 nonstandard extension used: nameless struct/union
REM wd4189 local variable is initialised but not referenced
REM wd4505 unreferenced local function not used will be removed

set CompileSwitches=-EHa -GR- -Oi -MTd -Z7 -W4 -wd4201 -wd4505 -Od
set Defines=

set CompileFlags=%CompileSwitches% /Fo%BinDir%\%ProjectName% /Fd%BinDir%\%ProjectName% /Fe%BinDir%\%ProjectName%
set DLLFlags=%CompileSwitches% /Fo%BinDir%\%ProjectNameDLL% /Fe%BinDir%\%ProjectNameDLL%

REM // Include Directories/Link Libraries
REM // =============================================================================================
set IncludeFiles=
set LinkLibraries=user32.lib Ole32.lib PortableDeviceGuids.lib
set DLLLinkLibraries=

REM incremental:no, turn incremental builds off
REM opt:ref,        try to remove functions from libs that are not referenced at all
set LinkFlags=-opt:ref -machine:x64 -nologo /DEBUG /NATVIS:External\Dqn.natvis

REM Clean time necessary for hours <10, which produces  H:MM:SS.SS where the
REM first character of time is an empty space. CleanTime will pad a 0 if
REM necessary.
set CleanTime=%time: =0%
set TimeStamp=%date:~6,4%%date:~3,2%%date:~0,2%_%CleanTime:~0,2%%CleanTime:~3,2%%CleanTime:~6,2%

REM // Compile
REM // =============================================================================================
del %BinDir%\*.pdb >NUL 2>NUL

REM cl %DLLFlags% %Defines% UnityBuildDLL.cpp %IncludeFiles% /LD /link /PDB:%BinDir%\%ProjectNameDLL%_%TimeStamp%.pdb %DLLLinkLibraries% %LinkFlags%

set LastError=%ERRORLEVEL%
if %LastError%==0 (
	cl %CompileFlags% %Defines% UnityBuild.cpp %IncludeFiles% /link %LinkLibraries% %LinkFlags%
	set LastError=%ERRORLEVEL%
)

:end
REM popd
if %HaveCTimeBinary%==1 ctime -end %ProjectName%.ctm %LastError%
REM ctime -stats %ProjectName%.ctm

exit /b %LastError%
