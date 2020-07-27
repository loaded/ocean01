
FILE=server

if [ -f "$FILE"];then
	rm "$FILE"
fi


gcc -o server minimal-ws-server.c -lwebsockets -ljansson

