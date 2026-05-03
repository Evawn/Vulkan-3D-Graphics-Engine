#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "FileDialog.h"

std::string FileDialog::OpenFile(
	const std::string& title,
	const std::vector<std::string>& filters,
	const std::string& defaultPath)
{
	@autoreleasepool {
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		[panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
		[panel setCanChooseFiles:YES];
		[panel setCanChooseDirectories:NO];
		[panel setAllowsMultipleSelection:NO];

		if (!filters.empty()) {
			NSMutableArray* types = [NSMutableArray array];
			for (const auto& f : filters) {
				[types addObject:[NSString stringWithUTF8String:f.c_str()]];
			}
			// Use UTType-based API on macOS 11+, fall back to allowedFileTypes
			if (@available(macOS 11.0, *)) {
				NSMutableArray<UTType*>* contentTypes = [NSMutableArray array];
				for (NSString* ext in types) {
					UTType* type = [UTType typeWithFilenameExtension:ext];
					if (type) [contentTypes addObject:type];
				}
				if (contentTypes.count > 0)
					[panel setAllowedContentTypes:contentTypes];
			} else {
				#pragma clang diagnostic push
				#pragma clang diagnostic ignored "-Wdeprecated-declarations"
				[panel setAllowedFileTypes:types];
				#pragma clang diagnostic pop
			}
		}

		if (!defaultPath.empty()) {
			NSString* dir = [[NSString stringWithUTF8String:defaultPath.c_str()] stringByDeletingLastPathComponent];
			[panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
		}

		if ([panel runModal] == NSModalResponseOK) {
			return std::string([[[panel URL] path] UTF8String]);
		}
		return "";
	}
}

std::string FileDialog::SaveFile(
	const std::string& title,
	const std::vector<std::string>& filters,
	const std::string& suggestedFileName,
	const std::string& defaultDirectory)
{
	@autoreleasepool {
		NSSavePanel* panel = [NSSavePanel savePanel];
		[panel setTitle:[NSString stringWithUTF8String:title.c_str()]];

		if (!filters.empty()) {
			NSMutableArray* types = [NSMutableArray array];
			for (const auto& f : filters) {
				[types addObject:[NSString stringWithUTF8String:f.c_str()]];
			}
			if (@available(macOS 11.0, *)) {
				NSMutableArray<UTType*>* contentTypes = [NSMutableArray array];
				for (NSString* ext in types) {
					UTType* type = [UTType typeWithFilenameExtension:ext];
					if (type) [contentTypes addObject:type];
				}
				if (contentTypes.count > 0)
					[panel setAllowedContentTypes:contentTypes];
			} else {
				#pragma clang diagnostic push
				#pragma clang diagnostic ignored "-Wdeprecated-declarations"
				[panel setAllowedFileTypes:types];
				#pragma clang diagnostic pop
			}
		}

		if (!suggestedFileName.empty()) {
			[panel setNameFieldStringValue:[NSString stringWithUTF8String:suggestedFileName.c_str()]];
		}
		if (!defaultDirectory.empty()) {
			[panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:defaultDirectory.c_str()]]];
		}

		if ([panel runModal] == NSModalResponseOK) {
			return std::string([[[panel URL] path] UTF8String]);
		}
		return "";
	}
}

std::string FileDialog::PickDirectory(
	const std::string& title,
	const std::string& defaultPath)
{
	@autoreleasepool {
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		[panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
		[panel setCanChooseFiles:NO];
		[panel setCanChooseDirectories:YES];
		[panel setCanCreateDirectories:YES];
		[panel setAllowsMultipleSelection:NO];

		if (!defaultPath.empty()) {
			[panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:defaultPath.c_str()]]];
		}

		if ([panel runModal] == NSModalResponseOK) {
			return std::string([[[panel URL] path] UTF8String]);
		}
		return "";
	}
}
