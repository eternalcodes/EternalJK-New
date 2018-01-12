#!/bin/sh

set -e
set -x

host="$1"
flavour="$2"
shift 2

if [[ "${host}" == macosx-universal-clang ]]; then
	brew install p7zip
	cd ~
	curl -O https://www.libsdl.org/release/SDL2-2.0.7.tar.gz
	tar xzf SDL2-2.0.7.tar.gz
	cd SDL2-2.0.7/Xcode/SDL
	sed -i -e 's/@rpath//g' SDL.xcodeproj/project.pbxproj
	xcodebuild ARCHS="i386 x86_64" ONLY_ACTIVE_ARCH=NO -configuration Release
	mkdir -p ~/Library/Frameworks/
	ln -s `pwd`/build/Release/SDL2.framework ~/Library/Frameworks/
else

# travis-ci's Ubuntu 14.04 image provides an apt source for Chrome,
# which breaks i386 multiarch. Disable it: we don't need it for any of
# these builds anyway.
: | sudo tee /etc/apt/sources.list.d/google-chrome.list

# do this before apt-get update
case "${host}" in
	(i?86-linux-gnu)
		sudo dpkg --add-architecture i386
		;;
esac

sudo apt-get update -qq
sudo apt-get -q -y install cmake dpkg p7zip-full

case "${host}" in
	(native)
		# upgrade some relevant libraries to vaguely modern versions
		sudo apt-get -q -y install libsdl2-dev libjpeg-turbo8-dev zlib1g-dev libpng12-dev libegl1-mesa-dev libgles2-mesa-dev
		;;

	(i686-w64-mingw32)
		sudo apt-get -q -y install g++-mingw-w64-i686
		;;

	(x86_64-w64-mingw32)
		sudo apt-get -q -y install g++-mingw-w64-x86-64
		;;

	(i?86-linux-gnu)
		# Install x86 libraries; remove anything that gets in the 
		# way, and also Java because that would be upgraded and is
		# quite large.
		sudo apt-get -q -y install \
			libgles2-mesa-dev:i386 libgl1-mesa-dev:i386 libglu1-mesa-dev:i386 libpulse-dev:i386 libglib2.0-dev:i386 \
			libsdl2-2.0-0:i386 libsdl2-dev:i386 libjpeg-turbo8-dev:i386 zlib1g-dev:i386 libc6-dev:i386 \
			libpng12-dev:i386 \
			g++-multilib g++-4.8-multilib gcc-4.8-multilib \
			g++ g++-4.8 gcc gcc-4.8 cpp cpp-4.8
		;;
esac
fi