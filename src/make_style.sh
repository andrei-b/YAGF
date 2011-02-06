#!/bin/sh
astyle --style=k\&r -l --convert-tabs --pad=oper --pad-header --indent-switches --unpad=paren --align-pointer=name --indent=spaces=4 *.cpp *.h 
