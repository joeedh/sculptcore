mergeInto(LibraryManager.library, {
    createMesh: function () {
        console.log("=>", this, arguments);
        Module.print('hello from lib!');
    }
});
