#!/bin/sh
# render-all.sh - demo renders of each effect into renders/.
# usage: tools/render-all.sh [input.wav]   (default: the synthetic test piece)
set -eu
cd "$(dirname "$0")/.."
mkdir -p renders
IN=${1:-renders/00-dry.wav}
[ $# -ge 1 ] || build/t/testpiece renders/00-dry.wav
R=build/t/mdrender
$R "$IN" renders/01-sp-292k.wav kbps=292
$R "$IN" renders/02-lp2-132k.wav kbps=132
$R "$IN" renders/03-lp4-66k-joint.wav kbps=66 stereo=joint
$R "$IN" renders/04-crushed-32k-joint.wav kbps=32 stereo=joint
$R "$IN" renders/05-preecho-long-blocks-96k.wav kbps=96 block=long
$R "$IN" renders/06-short-blocks-96k.wav kbps=96 block=short
$R "$IN" renders/07-lp2-8-generations.wav kbps=132 gens=8
$R "$IN" renders/08-lp4-4-generations.wav kbps=66 stereo=joint gens=4
$R "$IN" renders/09-skip-40.wav kbps=132 skip=40 jump=0
$R "$IN" renders/10-skip-90-jumps.wav kbps=132 skip=90 loop=6 jump=1.5
$R "$IN" renders/11-bumps.wav kbps=132 bump@2.5 bump@7 bump@11.5
$R "$IN" renders/12-freeze.wav kbps=132 loop=8 freeze@3.5-5.5 freeze@9-11 freeze@13-13.4
$R "$IN" renders/13-corrupt-70.wav kbps=132 corrupt=70
$R "$IN" renders/14-drift-100ppm.wav kbps=292 drift=100
$R "$IN" renders/15-drift-1500ppm-mix50.wav kbps=292 drift=1500 mix=50
$R "$IN" renders/16-wander-biterr.wav kbps=292 drift=60 wander=60 biterr=30
$R "$IN" renders/17-everything.wav kbps=66 stereo=joint gens=3 skip=35 corrupt=30 drift=250 wander=40 biterr=15 bump@8
echo "renders in $(pwd)/renders"
