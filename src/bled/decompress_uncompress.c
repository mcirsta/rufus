/* vi: set sw=4 ts=4: */
/*
 * uncompress for busybox -- (c) 2002 Robert Griebl
 *
 * based on the original compress42.c source
 * (see disclaimer below)
 */
/* (N)compress42.c - File compression ala IEEE Computer, Mar 1992.
 *
 * Authors:
 *   Spencer W. Thomas   (decvax!harpo!utah-cs!utah-gr!thomas)
 *   Jim McKie           (decvax!mcvax!jim)
 *   Steve Davies        (decvax!vax135!petsd!peora!srd)
 *   Ken Turkowski       (decvax!decwrl!turtlevax!ken)
 *   James A. Woods      (decvax!ihnp4!ames!jaw)
 *   Joe Orost           (decvax!vax135!petsd!joe)
 *   Dave Mack           (csu@alembic.acs.com)
 *   Peter Jannesen, Network Communication Systems
 *                       (peter@ncs.nl)
 *
 * marc@suse.de : a small security fix for a buffer overflow
 *
 * [... History snipped ...]
 */
#include "libbb.h"
#include "bb_archive.h"


/* Shared original dictionary/bitstream decoder; native hooks are opt-in. */
#include "uncompress_core.inc"
