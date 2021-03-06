/*
This file is part of cNVMe and is released under the MIT License
(C) - Charles Machalow - 2017
Controller.cpp - An implementation file for the NVMe Controller
*/

#define ADMIN_QUEUE_ID 0

#include "Command.h"
#include "Constants.h"
#include "Controller.h"
#include "PRP.h"
#include "Strings.h"

using namespace cnvme::command;

namespace cnvme
{
	namespace controller
	{
		Controller::Controller()
		{
			PCIExpressRegisters = new pci::PCIExpressRegisters();
			PCIExpressRegisters->waitForChangeLoop();

			pci::header::PCI_HEADER* PciHeader = PCIExpressRegisters->getPciExpressRegisters().PciHeader;
			UINT_64 BAR0Address = (UINT_64)PciHeader->MLBAR.BA + ((UINT_64)PciHeader->MUBAR.BA << 18);
			ControllerRegisters = new controller::registers::ControllerRegisters(BAR0Address, this); // Put the controller registers in BAR0/BAR1
			ControllerRegisters->waitForChangeLoop();

#ifndef SINGLE_THREADED
			DoorbellWatcher = LoopingThread([&] {Controller::checkForChanges(); }, CHANGE_CHECK_SLEEP_MS);
			DoorbellWatcher.start();
#endif

			// Todo: in the end, we'll need some form of media bank
		}

		Controller::~Controller()
		{
			DoorbellWatcher.end();

			if (PCIExpressRegisters)
			{
				delete PCIExpressRegisters;
				PCIExpressRegisters = nullptr;
			}

			if (ControllerRegisters)
			{
				delete ControllerRegisters;
				ControllerRegisters = nullptr;
			}
		}

		cnvme::controller::registers::ControllerRegisters* Controller::getControllerRegisters()
		{
			return ControllerRegisters;
		}

		pci::PCIExpressRegisters* Controller::getPCIExpressRegisters()
		{
			return PCIExpressRegisters;
		}

		void Controller::checkForChanges()
		{
			auto controllerRegisters = ControllerRegisters->getControllerRegisters();
			// Admin doorbell is right after the Controller Registers... though we may not have it yet
			controller::registers::QUEUE_DOORBELLS* doorbells = getControllerRegisters()->getQueueDoorbells();

			if (controllerRegisters->CSTS.RDY == 0)
			{
				return; // Not ready... Don't do anything.
			}

			if (controllerRegisters->ASQ.ASQB == 0)
			{
				return; // Don't have a admin submission queue address
			}

			// Now that we have a SQ address, make it valid
			if (ValidSubmissionQueues.size() == 0)
			{
				ValidSubmissionQueues.push_back(Queue(controllerRegisters->AQA.ASQS + 1, ADMIN_QUEUE_ID, &doorbells[ADMIN_QUEUE_ID].SQTDBL.SQT, controllerRegisters->ASQ.ASQB));
			}
			else
			{
				// Check if ASQB matches ValidSubmissionQueues
				Queue* adminQueue = getQueueWithId(ValidSubmissionQueues, ADMIN_QUEUE_ID);
				ASSERT_IF(!adminQueue, "Couldn't find the admin submission queue! Though ValidSubmissionQueue size is not 0.");
				adminQueue->setMemoryAddress(controllerRegisters->ASQ.ASQB);
			}

			if (controllerRegisters->ACQ.ACQB == 0)
			{
				return; // Don't have a admin completion queue address
			}

			// Now that we have a CQ address, make it valid
			if (ValidCompletionQueues.size() == 0)
			{
				Queue AdminCompletionQueue(controllerRegisters->AQA.ACQS + 1, ADMIN_QUEUE_ID, &doorbells[ADMIN_QUEUE_ID].CQHDBL.CQH, controllerRegisters->ACQ.ACQB);
				AdminCompletionQueue.setMappedQueue(&ValidSubmissionQueues[ADMIN_QUEUE_ID]); // Map CQ -> SQ
				ValidCompletionQueues.push_back(AdminCompletionQueue);

				Queue* adminSubQ = getQueueWithId(ValidSubmissionQueues, ADMIN_QUEUE_ID);
				ASSERT_IF(!adminSubQ, "Couldn't find the admin submission queue, to link it to the admin completion queue!");
				adminSubQ->setMappedQueue(&ValidCompletionQueues[ADMIN_QUEUE_ID]); // Map SQ -> CQ
			}
			else
			{
				// Check if ACQB matches ValidCompletionQueues
				Queue* adminQueue = getQueueWithId(ValidCompletionQueues, ADMIN_QUEUE_ID);
				ASSERT_IF(!adminQueue, "Couldn't find the admin completion queue! Though ValidCompletionQueues size is not 0.");
				adminQueue->setMemoryAddress(controllerRegisters->ACQ.ACQB);
			}

			// Made it this far, we have at least the admin queue
			// This is round-robin right now
			for (auto &sq : ValidSubmissionQueues)
			{
				if (doorbells[sq.getQueueId()].SQTDBL.SQT != sq.getTailPointer())
				{
					if (!sq.setTailPointer(doorbells[sq.getQueueId()].SQTDBL.SQT)) // Set our internal Queue instance's tail
					{
						LOG_ERROR("Should trigger AER since the Tail pointer given was invalid"); // Stop early.
						continue;
					}
					else
					{
						sq.getMappedQueue()->setTailPointer(sq.getTailPointer());  // Set in internal CQ as well
					}

					while (sq.getHeadPointer() != sq.getTailPointer())
					{
						processCommandAndPostCompletion(sq);
						sq.incrementAndGetHeadCloserToTail();
					}
				}
			}
		}

		void Controller::processCommandAndPostCompletion(Queue &submissionQueue)
		{
			Queue* theCompletionQueue = submissionQueue.getMappedQueue();
			if (theCompletionQueue == nullptr)
			{
				LOG_ERROR("Submission Queue " + std::to_string(submissionQueue.getQueueId()) + " doesn't have a mapped completion queue. And yet it recieved a command.");
				return;
			}

			UINT_8* subQPointer = (UINT_8*)submissionQueue.getMemoryAddress(); // This is the address of the 64 byte command
			NVME_COMMAND* command = (NVME_COMMAND*)subQPointer;

			command += submissionQueue.getHeadPointer();            // Make sure we get the correct command

			if (!isValidCommandIdentifier(command->DWord0Breakdown.CID, submissionQueue.getQueueId()))
			{
				COMPLETION_QUEUE_ENTRY cqe = { 0 };
				cqe.SC = constants::status::codes::generic::COMMAND_ID_CONFLICT; // Command ID Conflict 
				cqe.DNR = 1; // Do not retry
				postCompletion(*theCompletionQueue, cqe, command);
				return; // Do not process command since the CID/SQID combo was invalid;
			}

			Payload transferPayload;
			PRP prp;
			bool validCommand = true;
			COMPLETION_QUEUE_ENTRY completionQueueEntryToPost = { 0 };
			UINT_32 memoryPageSize = ControllerRegisters->getMemoryPageSize();

			if (memoryPageSize == 0)
			{
				LOG_ERROR("Unable to get memory page size. Did we lose the controller registers?");
				return;
			}

			if (submissionQueue.getQueueId() == ADMIN_QUEUE_ID)
			{
				// Admin command
				LOG_INFO(command->toString());

				switch (command->DWord0Breakdown.OPC)
				{
				case constants::opcodes::admin::IDENTIFY: // Identify
					LOG_INFO("Got an identify call!");

					// TODO : Check if ControllerRegisters is valid.
					prp = PRP(command->DPTR.DPTR1, command->DPTR.DPTR2, memoryPageSize, memoryPageSize);
					transferPayload = prp.getPayloadCopy();
					transferPayload.getBuffer()[0] = 1;
					transferPayload.getBuffer()[1] = 0xff;
					prp.placePayloadInExistingPRPs(transferPayload);
					break;
				case constants::opcodes::admin::KEEP_ALIVE: //Keep Alive... no data should be easiest
					break;

				default:
					validCommand = false;
				}

				if (validCommand)
				{
					postCompletion(*theCompletionQueue, completionQueueEntryToPost, command);
				}
				else
				{
					assert(0); // kill for now. Need to return invalid command opcode
					postCompletion(*theCompletionQueue, completionQueueEntryToPost, command);
				}

			}
			else
			{
				// NVM command
				assert(0); // Don't send me NVM commands yet.
			}
		}

		Queue* Controller::getQueueWithId(std::vector<Queue> &queues, UINT_16 id)
		{
			for (size_t i = 0; i < queues.size(); i++)
			{
				if (queues[i].getQueueId() == id)
				{
					return &queues[i];
				}
			}

			LOG_ERROR("Invalid queue id specified: " + std::to_string(id));
			return nullptr;
		}

		void Controller::postCompletion(Queue &completionQueue, COMPLETION_QUEUE_ENTRY completionEntry, NVME_COMMAND* command)
		{
			COMPLETION_QUEUE_ENTRY* completionQueueList = (COMPLETION_QUEUE_ENTRY*)MEMORY_ADDRESS_TO_8POINTER(completionQueue.getMemoryAddress());
			ASSERT_IF(completionQueueList == nullptr, "completionQueueList cannot be NULL");
			LOG_INFO("About to post completion to queue " + std::to_string(completionQueue.getQueueId()) + ". Head (just before moving): " + 
				std::to_string(completionQueue.getHeadPointer()) + ". Tail: " + std::to_string(completionQueue.getTailPointer()));

			completionQueueList += completionQueue.getHeadPointer(); // Move pointer to correct index

			Queue* submissionQueue = completionQueue.getMappedQueue();
			completionEntry.SQID = submissionQueue->getQueueId();
			completionEntry.SQHD = submissionQueue->getHeadPointer();
			completionEntry.CID = command->DWord0Breakdown.CID;

			bool phaseTag = false;
			auto node = QueueToPhaseTag.find(submissionQueue->getQueueId());
			if (node == QueueToPhaseTag.end())
			{
				QueueToPhaseTag[submissionQueue->getQueueId()] = false;    // start at false if not there... though it will flip to true below
			}
			else
			{
				phaseTag = QueueToPhaseTag[submissionQueue->getQueueId()]; // use existing
			}

			if (completionQueue.getHeadPointer() == 0) // need to flip
			{
				bool oldPhaseTag = QueueToPhaseTag[submissionQueue->getQueueId()];
				QueueToPhaseTag[submissionQueue->getQueueId()] = !oldPhaseTag;
				LOG_INFO("Inverting Phase Tag. Now Phase Tag == " + strings::toString(!oldPhaseTag));
			}
			phaseTag = QueueToPhaseTag[submissionQueue->getQueueId()]; // should be ready to be used (and flipped if needed).

			completionEntry.P = (UINT_16)phaseTag;

			UINT_32 completionQueueMemorySize = completionQueue.getQueueMemorySize();
			completionQueueMemorySize -= (completionQueue.getHeadPointer() * sizeof(COMPLETION_QUEUE_ENTRY)); // calculate new remaining memory size
			ASSERT_IF(completionQueueMemorySize < sizeof(COMPLETION_QUEUE_ENTRY), "completionQueueMemorySize must be greater than a single completion queue entry");
			memcpy_s(completionQueueList, completionQueueMemorySize, &completionEntry, sizeof(completionEntry)); // Post
			LOG_INFO(completionEntry.toString());

			completionQueue.incrementAndGetHeadCloserToTail(); // Move up CQ head

			// ring doorbell after placing data in completion queue.
			UINT_16* dbell = completionQueue.getDoorbell();
			dbell[0] = completionQueue.getHeadPointer();
		}

		bool Controller::isValidCommandIdentifier(UINT_16 commandId, UINT_16 submissionQueueId)
		{
			if (SubmissionQueueIdToCommandIdentifiers.find(submissionQueueId) == SubmissionQueueIdToCommandIdentifiers.end())
			{
				// SQID hasn't been used yet?
				std::set<UINT_16> tmp = { commandId };
				SubmissionQueueIdToCommandIdentifiers[submissionQueueId] = tmp;
				return true;
			}
			else
			{
				// Have SQID in SubmissionQueueIdToCommandIdentifiers
				std::set<UINT_16> &thisSubqsCids = SubmissionQueueIdToCommandIdentifiers[submissionQueueId];

				if (thisSubqsCids.size() == MAX_COMMAND_IDENTIFIER)
				{
					LOG_INFO("Made it to the end of possible CIDs for SubQ " + std::to_string(submissionQueueId) + ". Resetting possibile CIDs.");
					thisSubqsCids.clear();
				}

				if (thisSubqsCids.find(commandId) == thisSubqsCids.end())
				{
					thisSubqsCids.insert(commandId);
					return true;
				}
			}

			LOG_ERROR("Invalid command identifier was sent (" + std::to_string(commandId) + "). Was it re-used?");
			return false;
		}

		void Controller::controllerResetCallback()
		{
			LOG_INFO("Recv'd a controllerResetCallback request.");

			for (size_t i = ValidSubmissionQueues.size() - 1; i != -1; i--)
			{
				if (ValidSubmissionQueues[i].getQueueId() != ADMIN_QUEUE_ID)
				{
					ValidSubmissionQueues.erase(ValidSubmissionQueues.begin() + i);
				}
			}

			for (size_t i = ValidCompletionQueues.size() - 1; i != -1; i--)
			{
				if (ValidCompletionQueues[i].getQueueId() != ADMIN_QUEUE_ID)
				{
					ValidCompletionQueues.erase(ValidCompletionQueues.begin() + i);
				}
			}

			// Clear the SubQ to CID listing.
			this->SubmissionQueueIdToCommandIdentifiers.clear();

			// Clear phase tags.
			this->QueueToPhaseTag.clear();
		}

		void Controller::waitForChangeLoop()
		{
#ifndef SINGLE_THREADED
			DoorbellWatcher.waitForFlip();
#else
			checkForChanges();
#endif
		}
	}
}
