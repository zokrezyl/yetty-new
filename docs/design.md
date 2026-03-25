High level

-> we reduce the usage of smart_ptr where not needed. create (ObjectFactory) should return the pointer to type and not smart pointer anymore
-> the entire app is high level a tree of objects (yetty -> workspace -> pane -> terminalView -> terminalScreen -> etc etc
the deeper we go in the tree, the more "context objects" are needed. For this we will use context types that will just store pointers

GpuContext -> context everything related to webgpu handling (see current GpuContext)
YettyContext -> contains a gpuContext copy + config + other things may needed

terminalScreenContext -> the context created by an instance of terminalScreen and passed to child objects
