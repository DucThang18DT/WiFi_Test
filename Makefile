all:
	mkdir -p build
	g++ -std=c++17 ./*.cpp -o build/test

clean:
	rm -rf build/test
