#!/bin/bash

#
# version definition for uci
#
_uci_get_version()
{
    UCI_VERSION_FULL="$("${UCI_TOPDIR:=.}/scripts/kernel/setlocalversion" "${UCI_TOPDIR}/.tarball-version")"

    local orig_IFS="${IFS}"
    local IFS="."
    set -- ${UCI_VERSION_FULL}
    IFS="${orig_IFS}"

    UCI_VERSION_YEAR="${1}"
    UCI_VERSION_MONTH="${2}"
    UCI_VERSION_BUGFIX="${3%%-*}"
    UCI_VERSION_SCM="${3#${UCI_VERSION_BUGFIX}}"

    if [ -n "${UCI_VERSION_SCM}" ]; then
	UCI_VERSION_PTXRC="git"
    else
	UCI_VERSION_PTXRC="${UCI_VERSION_YEAR}.${UCI_VERSION_MONTH}"
    fi

}

_uci_get_version
