
#include <TUtil/TUtil.h>
#include <TUtil/vendor/libarchive/archive.h>
#include <TUtil/vendor/libarchive/archive_entry.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <iostream>
#include <mutex>
#include <utility>

using namespace TUtil;

struct ThreadStatus
{
	std::uint64_t BytesProcessed;
	bool Finished;
};

std::vector<ThreadStatus> s_Progress;
std::mutex s_ProgressMutex;

struct ThreadInfo
{
	std::size_t SectorSize;

	int ID;
	int ThreadCount;
	std::size_t SectorBegin, SectorEnd;
	std::unique_ptr<std::uint8_t[]> Buf;
	int fd;
};

ssize_t MyRead(struct archive *a, void *client_data, const void **buff)
{
	ThreadInfo* info = reinterpret_cast<ThreadInfo*>(client_data);

	*buff = info->Buf.get();
	return read(info->fd, info->Buf.get(), info->SectorSize);
}

int MyClose(struct archive *a, void *client_data)
{
	return (ARCHIVE_OK);
}

bool SaveArchive(struct archive* a)
{
	struct archive_entry* entry;
	while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
	{
		const char* name = archive_entry_pathname_utf8(entry);
		if (StringUtils::ContainsAny(name, "mca", "village", ".dat"))
		{
			Print::STDOUT << "Found good archive: " << name;
			Print::STDOUT.Flush();
			return true;
		}

	}
	return false;
}

void ResetArchive(struct archive*& a, ThreadInfo* info)
{
	a = archive_read_new();
	archive_read_support_filter_all(a);
	archive_read_support_format_zip(a);

	archive_read_set_read_callback(a, MyRead);
	archive_read_set_close_callback(a, MyClose);
	archive_read_set_callback_data(a, info);

}

static int copy_data(struct archive *ar, struct archive *aw)
{
	int r;
	const void *buff;
	size_t size;
	la_int64_t offset;

	while (true)
	{
		r = archive_read_data_block(ar, &buff, &size, &offset);
		if (r == ARCHIVE_EOF) return (ARCHIVE_OK);
		if (r < ARCHIVE_OK) return (r);
		r = archive_write_data_block(aw, buff, size, offset);
		if (r < ARCHIVE_OK)
		{
			fprintf(stderr, "%s\n", archive_error_string(aw));
			return (r);
		}
	}
}

void ArchiveLoop(ThreadInfo* info)
{

	Print::STDOUT << "Thread #" << info->ID << " starting!\nReading from " << info->SectorBegin << " to " << info->SectorEnd;
	Print::STDOUT.Flush();

	int i = 0;
	bool newArchive = true;
	struct archive* a = nullptr;
	for (std::size_t sector = info->SectorBegin; sector < info->SectorEnd; sector++)
	{
		s_ProgressMutex.lock();
		s_Progress[info->ID].BytesProcessed += info->SectorSize;
		s_ProgressMutex.unlock();

		std::size_t seekPos = sector * info->SectorSize;
		lseek(info->fd, seekPos, SEEK_SET);
		if (i % 10000 == 0)
		{
//			Print::STDOUT.W(info->ID).W(" at ").W(seekPos).W("\n").Flush();
		}
		i++;

		if (newArchive)
		{
			ResetArchive(a, info);
		}

		if (archive_read_open1(a) == ARCHIVE_OK)
		{
			bool saveArchive = SaveArchive(a);
			if (saveArchive)
			{
				fprintf(stdout, "SAVING ARCHIVE from thread %d\n", info->ID);
				//Seek back to the same position
				archive_read_close(a);
				archive_read_free(a);

				struct archive_entry* entry;
				int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS;

				struct archive* ext = archive_write_disk_new();
				archive_write_disk_set_options(ext, flags);
				archive_write_disk_set_standard_lookup(ext);

				lseek(info->fd, seekPos, SEEK_SET);
				ResetArchive(a, info);

				int r = archive_read_open1(a);
				if (r < ARCHIVE_OK) fprintf(stderr, "archive_read_open1 err: %s - #%d\n", archive_error_string(a), info->ID);
				if (r < ARCHIVE_WARN) exit(1);
				
				DefaultFormatter outDir;
				outDir << "./out/archive-" << seekPos << "/";
				FileSystem::CreateDirectories(outDir.c_str());

				while (true)
				{
					r = archive_read_next_header(a, &entry);
					if (r == ARCHIVE_EOF) break;
					if (r < ARCHIVE_OK) fprintf(stdout, "archive_read_next_header err: %s - #%d\n", archive_error_string(a), info->ID);
					if (r < ARCHIVE_WARN) break;

					const char* archivePath = archive_entry_pathname(entry);
					
					DefaultFormatter realName;
					realName << outDir.c_str() << archivePath;
					archive_entry_set_pathname(entry, realName.c_str());
					
					r = archive_write_header(ext, entry);
					if (r < ARCHIVE_OK) fprintf(stdout, "archive_write_header err: %s - #%d\n", archive_error_string(ext), info->ID);
					else
					{
						fprintf(stdout, "extracting %s - #%d\n", archivePath, info->ID);
						r = copy_data(a, ext);
						if (r < ARCHIVE_OK) fprintf(stdout, "copy_data err: %s - #%d\n", archive_error_string(ext), info->ID);
						if (r < ARCHIVE_WARN) break;
					}
					
					r = archive_write_finish_entry(ext);
					if (r < ARCHIVE_OK) fprintf(stdout, "archive_write_finish_entry err: %s - #%d\n", archive_error_string(ext), info->ID);
					if (r < ARCHIVE_WARN) exit(1);
				}
				archive_write_close(ext);
				archive_write_free(ext);

				fprintf(stdout, "\n======================================================================\n\nthread %d finished saving archive\n\n\n\n", info->ID);
			}

			archive_read_close(a);
			archive_read_free(a);

			newArchive = true;
		}
	}
	Print::STDOUT << "Thread #" << info->ID << " exiting! - read " << ((info->SectorEnd - info->SectorBegin) * info->SectorSize) << " bytes \n";
	Print::STDOUT.Flush();

	delete info;
	s_Progress[info->ID].Finished = true;
}

void sigpipe_handler(int unused)
{

}

int main()
{
	signal(SIGPIPE, sigpipe_handler);


	std::size_t length = 50L * 1000 * 1000 * 1000;
	std::size_t sectorSize = 4096;

	const char* driveName = "/dev/sdc";
	int threadCount = System::GetProcessorCount();
	threadCount = 1;
	Print::STDOUT << "Thread count " << threadCount << "\n";
	std::size_t sectorsPerThread = length / sectorSize / threadCount;
	s_Progress.resize(threadCount);

	std::vector<std::thread> threads;
	std::size_t currentSector = 0;
	for (int i = 0; i < threadCount; i++)
	{
		ThreadInfo* info = new ThreadInfo;
		info->SectorSize = sectorSize;
		info->ID = i;
		info->ThreadCount = threadCount;
		info->SectorBegin = currentSector;
		currentSector += sectorsPerThread;
		info->SectorEnd = currentSector;
		info->Buf.reset(new std::uint8_t[sectorSize]);
		info->fd = open(driveName, O_RDONLY);
		if (info->fd == -1)
		{
			Print::STDOUT << "Failed to open file " << driveName << "!";
			Print::STDOUT.Flush();
			return EXIT_FAILURE;
		}

		threads.emplace_back(ArchiveLoop, info);
	}
	setuid(1000);//Drop root perms
	Print::STDOUT.Flush();

	std::uint64_t total = 0;

	while (true)
	{
		bool done = true;
		for (int i = 0; i < threadCount; i++)
		{
			if (!s_Progress[i].Finished)
			{
				done = false;
				break;
			}
		}
		if (done) break;
		s_ProgressMutex.lock();
		Print::STDOUT << "Thread info ";
		std::uint64_t totalSpeed = 0;
		for (int i = 0; i < threadCount; i++)
		{
			total += s_Progress[i].BytesProcessed;
			totalSpeed += s_Progress[i].BytesProcessed;
			Print::STDOUT << (s_Progress[i].BytesProcessed / 1000.0 / 1000.0) << "MB/s";
			s_Progress[i].BytesProcessed = 0;
			if (i != threadCount - 1) Print::STDOUT << " | ";
		}
		s_ProgressMutex.unlock();
		Print::STDOUT.W(" === AVERAGE: ").W(totalSpeed / 1000.0 / 1000.0).W("MB/s -").W(-static_cast<double>(total) / length * 100.0).W("%\n").Flush();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

	}

	Print::STDOUT << "All threads complete \n";
	Print::STDOUT << "Read " << (length / 1000.0f / 1000.0f / 1000.0f) << " GB. Checked for " << (length / sectorSize) << " archives (every " << sectorSize << " bytes)\n";
	Print::STDOUT.Flush();

	return EXIT_SUCCESS;
}

