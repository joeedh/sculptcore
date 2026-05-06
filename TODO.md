* Remove buffers vector from DrawBatch
  - Buffers are logically either owned by client code or
    draw commands.
* Use shared ptrs for storing pointers to buffers.
* Create a mechanism for gpu resources to remove themselves from the
  arrays on GPUManager in their destructors.
