// Include after the game's headers in runtime sources: their globals go back to the ordinary
// sections, which a rollback does not restore.
#pragma data_seg()
#pragma bss_seg()
